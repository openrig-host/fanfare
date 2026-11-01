#include "StageRemoteModalOverlay.h"
#include "../BoutiqueLookAndFeel.h"
#include "../ButtonRoles.h"
#include "../HelpLauncher.h"
#include "../ThemeManager.h"

namespace OpenRig {

StageRemoteModalOverlay::StageRemoteModalOverlay(StageHotspotController& hotspotCtrl, int webServerPort)
    : hotspotController(hotspotCtrl), port(webServerPort) {
    setOpaque(false);
    const auto& t = ThemeManager::getInstance().active();

    closeBtn.setButtonText("CLOSE");
    Fanfare::setButtonRole(closeBtn, Fanfare::ButtonRole::Subtle);
    addAndMakeVisible(closeBtn);

    Fanfare::styleHelpButton(helpBtn, "#web-remote", "HAAALP! Open the Tablet Stage HUD & Wi-Fi Hotspot section of the User's Manual");
    addAndMakeVisible(helpBtn);
    closeBtn.onClick = [this]() {
        if (onClose) onClose();
    };

    // Wi-Fi Section
    wifiTitleLabel.setText("STAGE WI-FI SOFTAP (DIRECT LINK)", juce::dontSendNotification);
    wifiTitleLabel.setFont(BoutiqueLookAndFeel::condensedFont(14.5f, 0.06f));
    wifiTitleLabel.setColour(juce::Label::textColourId, t.iem);
    addAndMakeVisible(wifiTitleLabel);

    addAndMakeVisible(ssidLabel);
    ssidLabel.setFont(BoutiqueLookAndFeel::condensedFont(11.5f, 0.06f));
    ssidLabel.setColour(juce::Label::textColourId, t.engrave);

    addAndMakeVisible(ssidEditor);
    ssidEditor.setText(hotspotController.getSsid());
    ssidEditor.setFont(BoutiqueLookAndFeel::monoFont(12.5f, false));
    ssidEditor.setColour(juce::TextEditor::backgroundColourId, t.sunken);
    ssidEditor.setColour(juce::TextEditor::textColourId, t.text);
    ssidEditor.setColour(juce::TextEditor::outlineColourId, t.border);
    ssidEditor.onTextChange = [this]() { updateQrCodes(); };

    addAndMakeVisible(passLabel);
    passLabel.setFont(BoutiqueLookAndFeel::condensedFont(11.5f, 0.06f));
    passLabel.setColour(juce::Label::textColourId, t.engrave);

    addAndMakeVisible(passEditor);
    passEditor.setText(hotspotController.getPassword());
    passEditor.setFont(BoutiqueLookAndFeel::monoFont(12.5f, false));
    passEditor.setColour(juce::TextEditor::backgroundColourId, t.sunken);
    passEditor.setColour(juce::TextEditor::textColourId, t.text);
    passEditor.setColour(juce::TextEditor::outlineColourId, t.border);
    passEditor.onTextChange = [this]() { updateQrCodes(); };

    addAndMakeVisible(hotspotToggleBtn);
    Fanfare::setButtonRole(hotspotToggleBtn, Fanfare::ButtonRole::Primary);
    hotspotToggleBtn.onClick = [this]() {
        if (hotspotController.isActive()) {
            hotspotToggleBtn.setEnabled(false);
            hotspotController.stopHotspot([this](bool, juce::String) {
                hotspotToggleBtn.setEnabled(true);
            });
        } else {
            hotspotToggleBtn.setEnabled(false);
            hotspotController.startHotspot(ssidEditor.getText(), passEditor.getText(), [this](bool, juce::String) {
                hotspotToggleBtn.setEnabled(true);
            });
        }
    };

    addAndMakeVisible(hotspotStatusLabel);
    hotspotStatusLabel.setFont(BoutiqueLookAndFeel::uiFont(12.0f, false));
    hotspotStatusLabel.setColour(juce::Label::textColourId, t.textDim);

    addAndMakeVisible(wifiQrComponent);
    wifiQrCaption.setText("Scan with camera to Join Wi-Fi", juce::dontSendNotification);
    wifiQrCaption.setFont(BoutiqueLookAndFeel::uiFont(11.5f, false));
    wifiQrCaption.setJustificationType(juce::Justification::centred);
    wifiQrCaption.setColour(juce::Label::textColourId, t.textDim);
    addAndMakeVisible(wifiQrCaption);

    // Web Remote Section
    webTitleLabel.setText("TABLET STAGE COMPANION (WEB HUD)", juce::dontSendNotification);
    webTitleLabel.setFont(BoutiqueLookAndFeel::condensedFont(14.5f, 0.06f));
    webTitleLabel.setColour(juce::Label::textColourId, t.accent);
    addAndMakeVisible(webTitleLabel);

    primaryUrlLabel.setFont(BoutiqueLookAndFeel::monoFont(14.0f, true));
    primaryUrlLabel.setColour(juce::Label::textColourId, t.text);
    addAndMakeVisible(primaryUrlLabel);

    Fanfare::setButtonRole(copyUrlBtn, Fanfare::ButtonRole::Standard);
    addAndMakeVisible(copyUrlBtn);
    copyUrlBtn.onClick = [this]() {
        juce::SystemClipboard::copyTextToClipboard(primaryUrlLabel.getText());
        copyUrlBtn.setButtonText("COPIED");
        startTimer(1500);
    };

    Fanfare::setButtonRole(openBrowserBtn, Fanfare::ButtonRole::Primary);
    addAndMakeVisible(openBrowserBtn);
    openBrowserBtn.onClick = [this]() {
        juce::URL(primaryUrlLabel.getText()).launchInDefaultBrowser();
    };

    otherIpsLabel.setFont(BoutiqueLookAndFeel::monoFont(11.0f, false));
    otherIpsLabel.setColour(juce::Label::textColourId, t.textDim);
    addAndMakeVisible(otherIpsLabel);

    addAndMakeVisible(webQrComponent);
    webQrCaption.setText("Scan to Open Stage HUD in Browser", juce::dontSendNotification);
    webQrCaption.setFont(BoutiqueLookAndFeel::uiFont(11.5f, false));
    webQrCaption.setJustificationType(juce::Justification::centred);
    webQrCaption.setColour(juce::Label::textColourId, t.textDim);
    addAndMakeVisible(webQrCaption);

    usbInfoLabel.setText("USB Stage Link: Plug USB cable from laptop to tablet & enable USB Tethering for 0ms latency.", juce::dontSendNotification);
    usbInfoLabel.setFont(BoutiqueLookAndFeel::uiFont(12.0f, false));
    usbInfoLabel.setColour(juce::Label::textColourId, t.ok);
    addAndMakeVisible(usbInfoLabel);

    updateQrCodes();
    updateStatusDisplay(hotspotController.getState(), "");

    hotspotController.onStateChanged = [this](StageHotspotController::State st, const juce::String& msg) {
        updateStatusDisplay(st, msg);
    };
}

StageRemoteModalOverlay::~StageRemoteModalOverlay() {
    hotspotController.onStateChanged = nullptr;
}

void StageRemoteModalOverlay::timerCallback() {
    stopTimer();
    copyUrlBtn.setButtonText("COPY URL");
}

void StageRemoteModalOverlay::updateQrCodes() {
    juce::String ssid = ssidEditor.getText().trim();
    if (ssid.isEmpty()) ssid = "Fanfare-Stage";
    juce::String pass = passEditor.getText().trim();
    if (pass.isEmpty()) pass = "fanfare2026";

    wifiQrComponent.setPayload(QrCodeGenerator::makeWifiPayload(ssid, pass));

    auto ips = StageHotspotController::getAvailableIpAddresses();
    juce::String primaryIp = ips[0];
    if (hotspotController.isActive()) primaryIp = "192.168.137.1";
    juce::String url = "http://" + primaryIp + ":" + juce::String(port);

    primaryUrlLabel.setText(url, juce::dontSendNotification);
    webQrComponent.setPayload(url);

    juce::String otherIpsStr = "Available Network IPs: ";
    for (int i = 0; i < ips.size(); ++i) {
        if (i > 0) otherIpsStr += ", ";
        otherIpsStr += ips[i];
    }
    otherIpsLabel.setText(otherIpsStr, juce::dontSendNotification);
}

void StageRemoteModalOverlay::updateStatusDisplay(StageHotspotController::State state, const juce::String& msg) {
    const auto& t = ThemeManager::getInstance().active();
    if (state == StageHotspotController::State::Active) {
        hotspotToggleBtn.setButtonText("STOP STAGE HOTSPOT");
        hotspotToggleBtn.setToggleState(true, juce::dontSendNotification);
        Fanfare::setButtonRole(hotspotToggleBtn, Fanfare::ButtonRole::Mute);
        hotspotStatusLabel.setText("Status: ACTIVE (Broadcasting " + ssidEditor.getText() + ")", juce::dontSendNotification);
        hotspotStatusLabel.setColour(juce::Label::textColourId, t.ok);
    } else if (state == StageHotspotController::State::Starting) {
        hotspotToggleBtn.setButtonText("STARTING...");
        hotspotStatusLabel.setText("Status: Starting Windows Hotspot...", juce::dontSendNotification);
        hotspotStatusLabel.setColour(juce::Label::textColourId, t.warn);
    } else if (state == StageHotspotController::State::Stopping) {
        hotspotToggleBtn.setButtonText("STOPPING...");
        hotspotStatusLabel.setText("Status: Stopping...", juce::dontSendNotification);
        hotspotStatusLabel.setColour(juce::Label::textColourId, t.warn);
    } else if (state == StageHotspotController::State::Error) {
        hotspotToggleBtn.setButtonText("START STAGE HOTSPOT");
        hotspotToggleBtn.setToggleState(false, juce::dontSendNotification);
        Fanfare::setButtonRole(hotspotToggleBtn, Fanfare::ButtonRole::Primary);
        hotspotStatusLabel.setText("Error: " + msg, juce::dontSendNotification);
        hotspotStatusLabel.setColour(juce::Label::textColourId, t.danger);
    } else {
        hotspotToggleBtn.setButtonText("START STAGE HOTSPOT");
        hotspotToggleBtn.setToggleState(false, juce::dontSendNotification);
        Fanfare::setButtonRole(hotspotToggleBtn, Fanfare::ButtonRole::Primary);
        hotspotStatusLabel.setText("Status: Stopped (Offline)", juce::dontSendNotification);
        hotspotStatusLabel.setColour(juce::Label::textColourId, t.textDim);
    }
    updateQrCodes();
}

void StageRemoteModalOverlay::paint(juce::Graphics& g) {
    const auto& t = ThemeManager::getInstance().active();
    auto bounds = getLocalBounds();
    auto dialogArea = bounds.withSizeKeepingCentre(std::min(780, bounds.getWidth() - 30),
                                                   std::min(530, bounds.getHeight() - 30));

    Fanfare::ModalFrame::paintOverlayScrimAndPanel(
        g, bounds, dialogArea, "STAGE REMOTE & WI-FI HOTSPOT", 40);

    int midX = dialogArea.getX() + dialogArea.getWidth() / 2;
    g.setColour(t.hairlineStrong);
    g.drawVerticalLine(midX, (float)dialogArea.getY() + 48.0f, (float)dialogArea.getBottom() - 42.0f);

    g.setColour(t.hairline);
    g.drawHorizontalLine(dialogArea.getBottom() - 40, (float)dialogArea.getX() + 12.0f, (float)dialogArea.getRight() - 12.0f);
}

void StageRemoteModalOverlay::resized() {
    auto bounds = getLocalBounds();
    auto dialogArea = bounds.withSizeKeepingCentre(std::min(780, bounds.getWidth() - 30),
                                                   std::min(530, bounds.getHeight() - 30));

    auto titleArea = dialogArea.removeFromTop(40).reduced(10, 6);
    closeBtn.setBounds(titleArea.removeFromRight(56));
    titleArea.removeFromRight(4);
    helpBtn.setBounds(titleArea.removeFromRight(68));

    auto bottomArea = dialogArea.removeFromBottom(40);
    usbInfoLabel.setBounds(bottomArea.reduced(16, 8));

    int colWidth = dialogArea.getWidth() / 2;
    auto leftCol = dialogArea.removeFromLeft(colWidth).reduced(18, 12);
    auto rightCol = dialogArea.reduced(18, 12);

    // Left Column (Wi-Fi SoftAP)
    wifiTitleLabel.setBounds(leftCol.removeFromTop(24));
    leftCol.removeFromTop(6);

    auto ssidRow = leftCol.removeFromTop(28);
    ssidLabel.setBounds(ssidRow.removeFromLeft(75));
    ssidEditor.setBounds(ssidRow);

    leftCol.removeFromTop(6);
    auto passRow = leftCol.removeFromTop(28);
    passLabel.setBounds(passRow.removeFromLeft(75));
    passEditor.setBounds(passRow);

    leftCol.removeFromTop(8);
    hotspotToggleBtn.setBounds(leftCol.removeFromTop(32));
    hotspotStatusLabel.setBounds(leftCol.removeFromTop(22));

    leftCol.removeFromTop(6);
    auto wifiQrArea = leftCol.removeFromTop(130).withSizeKeepingCentre(130, 130);
    wifiQrComponent.setBounds(wifiQrArea);
    wifiQrCaption.setBounds(leftCol.removeFromTop(20));

    // Right Column (Web Companion HUD)
    webTitleLabel.setBounds(rightCol.removeFromTop(24));
    rightCol.removeFromTop(6);

    primaryUrlLabel.setBounds(rightCol.removeFromTop(28));

    auto btnRow = rightCol.removeFromTop(32);
    copyUrlBtn.setBounds(btnRow.removeFromLeft(btnRow.getWidth() / 2 - 4));
    openBrowserBtn.setBounds(btnRow.removeFromRight(btnRow.getWidth() - 4));

    otherIpsLabel.setBounds(rightCol.removeFromTop(26));

    rightCol.removeFromTop(6);
    auto webQrArea = rightCol.removeFromTop(130).withSizeKeepingCentre(130, 130);
    webQrComponent.setBounds(webQrArea);
    webQrCaption.setBounds(rightCol.removeFromTop(20));
}

} // namespace OpenRig
