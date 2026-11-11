import os
import subprocess
import urllib.request
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

    # 1. Get release info
    repo = "openrig-host/fanfare"
    tag = "v1.60"
    url = f"https://api.github.com/repos/{repo}/releases/tags/{tag}"
    req = urllib.request.Request(url, headers=base_headers)
    with urllib.request.urlopen(req) as resp:
        rel = json.loads(resp.read().decode("utf-8"))

    rel_id = rel["id"]
    upload_url_tmpl = rel["upload_url"]
    base_upload_url = upload_url_tmpl.split("{")[0]

    print(f"Release: {rel['name']} (ID: {rel_id})")

    # 2. Delete old MSI and EXE assets
    to_delete = ["Fanfare-1.60.0-x64.msi", "Fanfare.exe", "Fanfare-v1.60-win64-full.zip"]
    for asset in rel.get("assets", []):
        if asset["name"] in to_delete:
            aid = asset["id"]
            print(f"Deleting outdated asset: {asset['name']} (ID: {aid})...")
            d_req = urllib.request.Request(
                f"https://api.github.com/repos/{repo}/releases/assets/{aid}",
                headers=base_headers,
                method="DELETE"
            )
            with urllib.request.urlopen(d_req) as d_resp:
                pass
            print("Deleted successfully.")

    # 3. Upload new assets
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    assets_to_upload = [
        (os.path.join(repo_root, "installer", "output", "Fanfare-1.60.0-x64.msi"), "Fanfare-1.60.0-x64.msi", "application/octet-stream"),
        (os.path.join(repo_root, "Fanfare.exe"), "Fanfare.exe", "application/octet-stream"),
        (os.path.join(repo_root, "installer", "output", "Fanfare-v1.60-win64-full.zip"), "Fanfare-v1.60-win64-full.zip", "application/zip")
    ]

    for fpath, name, ctype in assets_to_upload:
        if not os.path.exists(fpath):
            print(f"WARNING: File not found: {fpath}")
            continue
        size = os.path.getsize(fpath)
        print(f"Uploading {name} ({size} bytes)...")
        upload_headers = dict(base_headers)
        upload_headers["Content-Type"] = ctype
        upload_headers["Content-Length"] = str(size)
        upload_url = f"{base_upload_url}?name={name}"

        with open(fpath, "rb") as fp:
            data = fp.read()

        u_req = urllib.request.Request(upload_url, data=data, headers=upload_headers, method="POST")
        with urllib.request.urlopen(u_req) as u_resp:
            result = json.loads(u_resp.read().decode("utf-8"))
            print(f"Uploaded {name} successfully (Asset ID: {result['id']}).")

    print("\nAll assets updated successfully!")

if __name__ == "__main__":
    main()
