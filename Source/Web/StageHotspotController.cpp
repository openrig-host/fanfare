#include "StageHotspotController.h"

namespace OpenRig {

namespace {
    juce::String sanitizeHotspotField(const juce::String& input, int maxChars) {
        juce::String cleaned;
        cleaned.preallocateBytes((size_t)maxChars * 2);
        int count = 0;
        for (juce::juce_wchar ch : input) {
            if (count >= maxChars)
                break;
            if (ch < 0x20 || ch == 0x7f)
                continue;
            cleaned += juce::String::charToString(ch);
            ++count;
        }
        return cleaned.trim();
    }

    juce::String escapePsSingleQuoted(const juce::String& s) {
        return s.replace("'", "''");
    }
}

StageHotspotController::StageHotspotController()
    : juce::Thread("StageHotspotWorker") {
}

StageHotspotController::~StageHotspotController() {
    if (aliveToken)
        aliveToken->store(false, std::memory_order_release);
    signalThreadShouldExit();
    notify();
    stopThread(-1);

    std::function<void(bool, juce::String)> leftoverCb;
    {
        std::lock_guard<std::mutex> lock(taskMutex);
        leftoverCb = std::move(pendingCallback);
        pendingCallback = nullptr;
    }
    if (leftoverCb)
        leftoverCb(false, "Hotspot controller shut down.");
}

void StageHotspotController::startHotspot(const juce::String& ssid,
                                          const juce::String& password,
                                          std::function<void(bool, juce::String)> onFinished) {
    std::function<void(bool, juce::String)> displacedCb;
    {
        std::lock_guard<std::mutex> lock(taskMutex);
        const juce::String cleanSsid = sanitizeHotspotField(ssid, 32);
        const juce::String cleanPass = sanitizeHotspotField(password, 63);
        activeSsid = cleanSsid.isNotEmpty() ? cleanSsid : "Fanfare-Stage";
        activePassword = cleanPass.length() >= 8 ? cleanPass : "fanfare2026";
        displacedCb = std::move(pendingCallback);
        pendingCallback = std::move(onFinished);
        pendingTask = TaskType::Start;
        currentState = State::Starting;
    }

    if (displacedCb) {
        juce::MessageManager::callAsync([displacedCb]() {
            displacedCb(false, "Superseded by newer hotspot request.");
        });
    }

    if (onStateChanged) {
        auto alive = aliveToken;
        juce::MessageManager::callAsync([this, alive]() {
            if (!alive || !alive->load(std::memory_order_acquire))
                return;
            if (onStateChanged)
                onStateChanged(State::Starting, "Starting Stage Wi-Fi Hotspot...");
        });
    }

    if (!isThreadRunning()) {
        startThread();
    } else {
        notify();
    }
}

void StageHotspotController::stopHotspot(std::function<void(bool, juce::String)> onFinished) {
    std::function<void(bool, juce::String)> displacedCb;
    {
        std::lock_guard<std::mutex> lock(taskMutex);
        displacedCb = std::move(pendingCallback);
        pendingCallback = std::move(onFinished);
        pendingTask = TaskType::Stop;
        currentState = State::Stopping;
    }

    if (displacedCb) {
        juce::MessageManager::callAsync([displacedCb]() {
            displacedCb(false, "Superseded by newer hotspot request.");
        });
    }

    if (onStateChanged) {
        auto alive = aliveToken;
        juce::MessageManager::callAsync([this, alive]() {
            if (!alive || !alive->load(std::memory_order_acquire))
                return;
            if (onStateChanged)
                onStateChanged(State::Stopping, "Stopping Stage Wi-Fi Hotspot...");
        });
    }

    if (!isThreadRunning()) {
        startThread();
    } else {
        notify();
    }
}

void StageHotspotController::run() {
    while (!threadShouldExit()) {
        TaskType task = pendingTask.exchange(TaskType::None);
        if (task == TaskType::None) {
            wait(500);
            continue;
        }

        juce::String currentSsid, currentPassword;
        std::function<void(bool, juce::String)> cb;
        {
            std::lock_guard<std::mutex> lock(taskMutex);
            currentSsid = activeSsid;
            currentPassword = activePassword;
            cb = std::move(pendingCallback);
            pendingCallback = nullptr;
        }

        bool isStarting = (task == TaskType::Start);
        juce::String outMessage;
        bool success = executeWindowsHotspotScript(isStarting, currentSsid, currentPassword, outMessage);

        State newState = isStarting ? (success ? State::Active : State::Error)
                                    : (success ? State::Stopped : State::Error);
        currentState = newState;
        lastMessage = outMessage;

        auto alive = aliveToken;
        juce::MessageManager::callAsync([this, alive, newState, outMessage, cb, success]() {
            if (alive && alive->load(std::memory_order_acquire)) {
                if (onStateChanged)
                    onStateChanged(newState, outMessage);
            }
            if (cb)
                cb(success, outMessage);
        });
    }
}

bool StageHotspotController::executeWindowsHotspotScript(bool enable,
                                                         const juce::String& ssid,
                                                         const juce::String& password,
                                                         juce::String& outMessage) {
#if JUCE_WINDOWS
    const juce::String safeSsid = escapePsSingleQuoted(sanitizeHotspotField(ssid, 32));
    const juce::String safePass = escapePsSingleQuoted(sanitizeHotspotField(password, 63));

    juce::File scriptFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile("openrig_stage_hotspot_" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()) + ".ps1");

    juce::String psScript;
    psScript << "[CmdletBinding()]\n";
    psScript << "param([string]$Action = '" << (enable ? "start" : "stop") << "',\n";
    psScript << "      [string]$SSID = '" << safeSsid << "',\n";
    psScript << "      [string]$Password = '" << safePass << "')\n\n";

    psScript << "Add-Type -AssemblyName System.Runtime.WindowsRuntime\n";
    psScript << "$asTaskGeneric = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' }[0]\n\n";

    psScript << "Function AwaitResult($WinRtTask, $ResultType) {\n";
    psScript << "    $asTask = $asTaskGeneric.MakeGenericMethod($ResultType)\n";
    psScript << "    $netTask = $asTask.Invoke($null, @($WinRtTask))\n";
    psScript << "    $netTask.Wait(12000) | Out-Null\n";
    psScript << "    return $netTask.Result\n";
    psScript << "}\n\n";

    psScript << "$connProfile = [Windows.Networking.Connectivity.NetworkInformation, Windows.Networking.Connectivity, ContentType = WindowsRuntime]::GetInternetConnectionProfile()\n";
    psScript << "if (-not $connProfile) {\n";
    psScript << "    $profiles = [Windows.Networking.Connectivity.NetworkInformation, Windows.Networking.Connectivity, ContentType = WindowsRuntime]::GetConnectionProfiles()\n";
    psScript << "    if ($profiles.Count -gt 0) { $connProfile = $profiles[0] }\n";
    psScript << "}\n\n";

    psScript << "if (-not $connProfile) {\n";
    psScript << "    Write-Output 'ERROR: No active network adapter found on Windows.'\n";
    psScript << "    exit 1\n";
    psScript << "}\n\n";

    psScript << "$manager = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager, Windows.Networking.NetworkOperators, ContentType = WindowsRuntime]::CreateFromConnectionProfile($connProfile)\n\n";

    psScript << "if ($Action -eq 'start') {\n";
    psScript << "    try {\n";
    psScript << "        $config = $manager.GetCurrentAccessPointConfiguration()\n";
    psScript << "        if ($config) {\n";
    psScript << "            $config.Ssid = $SSID\n";
    psScript << "            $config.Passphrase = $Password\n";
    psScript << "            $manager.ConfigureAccessPointAsync($config)\n";
    psScript << "        }\n";
    psScript << "    } catch {}\n\n";
    psScript << "    $res = AwaitResult ($manager.StartTetheringAsync()) ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])\n";
    psScript << "    if ($res.Status -eq [Windows.Networking.NetworkOperators.TetheringOperationStatus]::Success) {\n";
    psScript << "        Write-Output ('SUCCESS: Hotspot active. SSID: ' + $SSID)\n";
    psScript << "        exit 0\n";
    psScript << "    } else {\n";
    psScript << "        Write-Output ('ERROR: ' + $res.Status)\n";
    psScript << "        exit 1\n";
    psScript << "    }\n";
    psScript << "} elseif ($Action -eq 'stop') {\n";
    psScript << "    $res = AwaitResult ($manager.StopTetheringAsync()) ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])\n";
    psScript << "    Write-Output 'SUCCESS: Hotspot stopped'\n";
    psScript << "    exit 0\n";
    psScript << "}\n";

    if (!scriptFile.replaceWithText(psScript)) {
        outMessage = "Could not write temporary PowerShell script.";
        return false;
    }

    juce::ChildProcess proc;
    juce::StringArray args;
    args.add("powershell.exe");
    args.add("-NoProfile");
    args.add("-ExecutionPolicy");
    args.add("Bypass");
    args.add("-WindowStyle");
    args.add("Hidden");
    args.add("-File");
    args.add(scriptFile.getFullPathName());

    if (proc.start(args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) {
        juce::MemoryOutputStream outBuf;
        const juce::uint32 startMs = juce::Time::getMillisecondCounter();
        constexpr juce::uint32 kTimeoutMs = 15000;
        bool timedOutOrAborted = false;

        char chunk[1024];
        while (proc.isRunning()) {
            for (;;) {
                const int n = proc.readProcessOutput(chunk, (int)sizeof(chunk));
                if (n <= 0)
                    break;
                outBuf.write(chunk, (size_t)n);
            }

            if (threadShouldExit() || (juce::Time::getMillisecondCounter() - startMs) >= kTimeoutMs) {
                timedOutOrAborted = true;
                proc.kill();
                proc.waitForProcessToFinish(1000);
                break;
            }
            juce::Thread::sleep(50);
        }

        if (!timedOutOrAborted) {
            for (;;) {
                const int n = proc.readProcessOutput(chunk, (int)sizeof(chunk));
                if (n <= 0)
                    break;
                outBuf.write(chunk, (size_t)n);
            }
        }

        scriptFile.deleteFile();

        if (timedOutOrAborted) {
            outMessage = threadShouldExit() ? "Hotspot operation cancelled." : "Timed out waiting for Windows Mobile Hotspot.";
            return false;
        }

        const juce::String output = outBuf.toString().trim();
        if (output.containsIgnoreCase("SUCCESS")) {
            outMessage = output;
            return true;
        } else {
            outMessage = output.isNotEmpty() ? output : "Failed to start Windows Mobile Hotspot.";
            return false;
        }
    }

    scriptFile.deleteFile();
    outMessage = "Could not launch PowerShell process.";
    return false;
#else
    outMessage = "Stage Hotspot controller is supported on Windows.";
    return false;
#endif
}

juce::StringArray StageHotspotController::getAvailableIpAddresses() {
    juce::StringArray preferredLanIps;
    juce::StringArray otherUnicastIps;
    auto addrs = juce::IPAddress::getAllAddresses();
    for (const auto& addr : addrs) {
        if (addr.isNull() || addr.isIPv6)
            continue;
        const uint8_t a = addr.address[0];
        const uint8_t b = addr.address[1];
        // Exclude 0.x.x.x, loopback 127.x.x.x, dead link-local APIPA 169.254.x.x, and multicast/broadcast 224..255.x.x.x
        if (a == 0 || a == 127 || (a == 169 && b == 254) || a >= 224)
            continue;

        const juce::String s = addr.toString();
        const bool isPrivateLan = (a == 192 && b == 168)
                               || (a == 10)
                               || (a == 172 && b >= 16 && b <= 31);
        if (isPrivateLan)
            preferredLanIps.addIfNotAlreadyThere(s);
        else
            otherUnicastIps.addIfNotAlreadyThere(s);
    }

    juce::StringArray ips;
    ips.addArray(preferredLanIps);
    ips.addArray(otherUnicastIps);
    if (ips.isEmpty()) {
        ips.add("192.168.137.1"); // Windows default hotspot IP
    }
    return ips;
}

} // namespace OpenRig
