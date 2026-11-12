import os
import subprocess
import urllib.request
import urllib.error
import json

def get_github_token():
    p = subprocess.run(
        ["git", "credential", "fill"],
        input="protocol=https\nhost=github.com\nusername=dwaugh-edsim\n",
        text=True,
        capture_output=True,
        check=True
    )
    for line in p.stdout.splitlines():
        if line.startswith("password="):
            return line[9:].strip()
    raise RuntimeError("Could not obtain token from git credential helper")

def main():
    token = get_github_token()
    base_headers = {
        "Authorization": f"Bearer {token}",
        "Accept": "application/vnd.github+json",
        "User-Agent": "Fanfare-Release-Script"
    }

    repo = "openrig-host/fanfare"
    tag = "v1.61"
    url = f"https://api.github.com/repos/{repo}/releases/tags/{tag}"

    release_body = """## Fanfare v1.61.0 — Stability, UAF Prevention & Strip Preset Bundle

This is a critical maintenance and stability release addressing UI-thread heap corruption, audio engine resilience, and expanding the bundled factory preset library.

### Dev Blog
Read Dave's companion field notes from the stage:  
**[On Glitches: Buffer greed, thermal traps, Windows power schemes, and Fanfare's in-flight data recorder](https://openrig-host.github.io/fanfare/blog/2026-11-12-on-glitches.html)**

---

### Bug Fixes & Stability
- **PluginEditor Lifetime & UAF Prevention**: Resolved a deterministic `EXCEPTION_ACCESS_VIOLATION` in `VCRUNTIME140.dll` caused by `PluginWindow` content component destruction races. Added an `onBeforePluginDestroyed` hook to synchronously dismiss and tear down active UI editors before plugin instances are unhooked or replaced.
- **Audio Engine Hardening**: Wrapped `FanfareEngine::prepareToPlay()` in a `ScopedAudioQuietWindow` to prevent sample rate and buffer size configuration races on ASIO device initialization.
- **Panic Flush Optimization**: Optimized real-time `panicTriggered` audio thread logic to avoid synchronously zeroing >100 MB of reverb and delay line memory in a single callback block, eliminating 22+ ms callback overruns.
- **USB & ACPI Stall Mitigation**: Filtered noisy `DBT_DEVNODES_CHANGED` Win32 PnP messages and relaxed ACPI thermal query polling intervals (from 2s to 5s) to eliminate DPC spikes.
- **Flight Recorder Filtering**: Filtered out multi-hour sleep/hibernate timestamp jumps from `audio_underrun_log.txt`.

### Bundled Factory Content
- **New Strip Presets (`.orstrip`)**: Bundled 6 gig-tested strip configurations:
  - `Arp Heaven.orstrip`
  - `04 Glocken & Enya Chime.orstrip`
  - `flute+fmxylo.orstrip`
  - `accordion-input.orstrip`
  - `handclap sample.orstrip`
  - `Mp3 Gig Strip.orstrip`
- **Native Synthesizer Presets**: Added 22 analog synth patches (`BLEEP-216`) and 3 FM synth patches (`CoplanDX`).
- **Asset Generation**: Integrated presets into binary embedded assets and installer WiX manifests (`BundledPresets.wxs`).

### Downloads
- **`Fanfare-1.61.0-x64.msi`**: Standard Windows 64-bit installer with desktop and start menu shortcuts.
- **`Fanfare-1.61.0-x64.msix`**: Windows App Package (MSIX) for modern sideloading.
- **`Fanfare-v1.61-win64-full.zip`**: Portable release archive with executable and complete factory presets.
- **`Fanfare.exe`**: Standalone executable (x64).
"""

    rel = None
    try:
        req = urllib.request.Request(url, headers=base_headers)
        with urllib.request.urlopen(req) as resp:
            rel = json.loads(resp.read().decode("utf-8"))
            print(f"Found existing release for {tag} (ID: {rel['id']})")
    except urllib.error.HTTPError as e:
        if e.code == 404:
            print(f"No existing release for {tag}. Creating a new release...")
        else:
            raise

    if rel is None:
        create_payload = {
            "tag_name": tag,
            "target_commitish": "trunk",
            "name": "Fanfare v1.61.0 — Stability & Preset Bundle",
            "body": release_body,
            "draft": False,
            "prerelease": False
        }
        create_req = urllib.request.Request(
            f"https://api.github.com/repos/{repo}/releases",
            data=json.dumps(create_payload).encode("utf-8"),
            headers={**base_headers, "Content-Type": "application/json"},
            method="POST"
        )
        with urllib.request.urlopen(create_req) as c_resp:
            rel = json.loads(c_resp.read().decode("utf-8"))
            print(f"Created release '{rel['name']}' (ID: {rel['id']})")
    else:
        # Update body and name if needed
        update_payload = {
            "name": "Fanfare v1.61.0 — Stability & Preset Bundle",
            "body": release_body
        }
        u_req = urllib.request.Request(
            f"https://api.github.com/repos/{repo}/releases/{rel['id']}",
            data=json.dumps(update_payload).encode("utf-8"),
            headers={**base_headers, "Content-Type": "application/json"},
            method="PATCH"
        )
        with urllib.request.urlopen(u_req) as u_resp:
            rel = json.loads(u_resp.read().decode("utf-8"))
            print(f"Updated release metadata for ID: {rel['id']}")

    rel_id = rel["id"]
    upload_url_tmpl = rel["upload_url"]
    base_upload_url = upload_url_tmpl.split("{")[0]

    # Delete existing assets if any conflict
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    assets_to_upload = [
        (os.path.join(repo_root, "installer", "output", "Fanfare-1.61.0-x64.msi"), "Fanfare-1.61.0-x64.msi", "application/octet-stream"),
        (os.path.join(repo_root, "installer", "output", "Fanfare-1.61.0-x64.msix"), "Fanfare-1.61.0-x64.msix", "application/octet-stream"),
        (os.path.join(repo_root, "installer", "output", "Fanfare-v1.61-win64-full.zip"), "Fanfare-v1.61-win64-full.zip", "application/zip"),
        (os.path.join(repo_root, "Fanfare.exe"), "Fanfare.exe", "application/octet-stream"),
    ]

    existing_asset_names = {a["name"] for a in rel.get("assets", [])}
    for fpath, name, ctype in assets_to_upload:
        if name in existing_asset_names:
            print(f"Asset already present: {name} (skipping upload)")
            continue
        if not os.path.exists(fpath):
            print(f"WARNING: File not found: {fpath}")
            continue
        size = os.path.getsize(fpath)
        print(f"Uploading {name} ({size / (1024*1024):.2f} MB)...")
        upload_headers = dict(base_headers)
        upload_headers["Content-Type"] = ctype
        upload_headers["Content-Length"] = str(size)
        upload_url = f"{base_upload_url}?name={name}"

        with open(fpath, "rb") as fp:
            data = fp.read()

        up_req = urllib.request.Request(upload_url, data=data, headers=upload_headers, method="POST")
        with urllib.request.urlopen(up_req) as up_resp:
            result = json.loads(up_resp.read().decode("utf-8"))
            print(f"Uploaded {name} successfully (Asset ID: {result['id']}).")

    print(f"\nRelease {tag} published successfully!")
    print(f"URL: {rel['html_url']}")

if __name__ == "__main__":
    main()

