# Microsoft Store Submission Guide (Fanfare)

This directory contains everything needed to package Fanfare as **both** a standalone x64 Windows Installer (`.msi`) and a Microsoft Store Desktop Bridge package (`.msix`).

---

## Option A — MSIX Upload ("MSIX or PWA" App Slot — Free Microsoft Code Signing, No External URL)

If you create your app in [Microsoft Partner Center](https://partner.microsoft.com/dashboard) as **MSIX or PWA**, Microsoft signs both the `.msix` and `Fanfare.exe` **for free** upon certification, and you upload the `.msix` directly (no external download URL required).

### Step 1: Copy Your Assigned Product Identity
In Partner Center, open your app → **Product management → Product identity** and copy:
- **Package/Identity/Name** *(e.g. `12345YourPublisher.Fanfare`)*
- **Package/Identity/Publisher** *(e.g. `CN=12345678-ABCD-EF01-2345-6789ABCDEF01`)*
- **Package/Properties/PublisherDisplayName** *(your Partner Center publisher display name)*

### Step 2: Stamp & Build the `.msix`
```powershell
powershell -ExecutionPolicy Bypass -File tools\build_installers.ps1 `
    -ExePath "path\to\Fanfare.exe" `
    -Version 1.51.0 `
    -DisplayVersion 1.51 `
    -Format Msix `
    -StoreIdentityName "PASTE_IDENTITY_NAME_HERE" `
    -StorePublisher "CN=PASTE_PUBLISHER_GUID_HERE" `
    -StorePublisherDisplayName "PASTE_PUBLISHER_DISPLAY_NAME_HERE"
```

Upload `installer\output\Fanfare-1.51.0-x64.msix` directly into **Packages** in Partner Center.

### Step 3: Copy-Paste `runFullTrust` Justification
Under **Submission options → Restricted capabilities (`runFullTrust`)**, paste:
> Fanfare is a desktop real-time digital audio workstation / live VST3 instrument host for stage musicians. It requires `runFullTrust` to load user-installed third-party VST3 audio plug-ins from `C:\Program Files\Common Files\VST3`, access low-latency hardware ASIO audio drivers registered in Windows, and read/write user setlists and audio presets on disk.

---

## Option B — Win32 `.msi` Installer ("EXE or MSI" App Slot — Direct Non-Redirect HTTPS URL)

If your Partner Center listing was created as **EXE or MSI**:

1. **Non-Redirecting Package URL (GitHub Pages — HTTP `200 OK`, zero redirects):**
   ```text
   https://openrig-host.github.io/fanfare/downloads/Fanfare-1.51.0-x64.msi
   ```
   *(Never overwrite an existing versioned filename — for future updates, build `-Version 1.52.0 -PublishToDocs` to publish a new immutable URL.)*

2. **Partner Center Installer Parameters:**
   - **Installer parameters (silent install):** `/quiet /norestart`
   - **Architecture:** `x64`
   - **Installer type:** `MSI`

3. **Code Signing Requirement (Mandatory for Win32 `.msi` / `.exe`):**
   Partner Center requires external `.msi` packages and the `Fanfare.exe` inside them to be Authenticode-signed with a certificate chaining to the Microsoft Trusted Root Program (for example, Azure Artifact/Trusted Signing or an OV Code Signing certificate). Pass `-CertThumbprint "<SHA1>"` or `-PfxPath "cert.pfx" -PfxPassword "..."` to `tools\build_installers.ps1` to sign both `Fanfare.exe` and the `.msi` automatically.

---

## Privacy Policy URL (Required for All Store Listings)
```text
https://openrig-host.github.io/fanfare/privacy.html
```
