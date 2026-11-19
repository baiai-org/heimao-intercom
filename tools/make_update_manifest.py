"""Sign the update.json asset for the official GitHub Release."""
import argparse
import base64
import hashlib
import json
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PUBLIC_KEY = ROOT / "firmware/main/release_update_public.pem"
TARGETS = {"firmware": "heimao-esp32s3-n16r8", "app": "net.openradiolink.app"}
NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,95}")


def command(openssl, *args, **kwargs):
    return subprocess.run([openssl, *map(str, args)], check=True, **kwargs)


def load_previous(openssl, path):
    envelope = json.loads(path.read_bytes())
    payload = base64.b64decode(envelope["payload"], validate=True)
    signature = base64.b64decode(envelope["signature"], validate=True)
    with tempfile.TemporaryDirectory() as folder:
        sig = Path(folder) / "signature"
        sig.write_bytes(signature)
        command(openssl, "dgst", "-sha256", "-verify", PUBLIC_KEY, "-signature", sig,
                input=payload, stdout=subprocess.PIPE)
    data = json.loads(payload)
    if data.get("schema") != 1:
        raise ValueError("unsupported manifest schema")
    return data


def asset(component, item):
    path = Path(item[0])
    tag, code, label = item[1:]
    sequence = int(code)
    if not all(NAME.fullmatch(s) for s in [tag, path.name, label]) or not 0 < sequence <= 2147483647:
        raise ValueError("invalid tag, asset name, or version")
    length = path.stat().st_size
    if component == "firmware":
        if label != f"r11-{sequence}" or not path.name.endswith(".bin") or not 65536 <= length <= 4194304:
            raise ValueError("expected an r11 OTA application image with its matching numeric version")
        with path.open("rb") as file:
            header = file.read(80)
        if header[0] != 0xe9 or header[12:14] != b"\x09\x00" or header[32:36] != b"\x32\x54\xcd\xab":
            raise ValueError("expected an ESP32-S3 app image, not a full flash image")
        if header[48:80].split(b"\x00", 1)[0].decode("ascii").split("-g", 1)[0] != label:
            raise ValueError("firmware image reports a different version")
    elif not path.name.endswith(".apk") or not 0 < length <= 104857600:
        raise ValueError("expected an APK under 100 MiB")
    with path.open("rb") as file:
        digest = hashlib.file_digest(file, "sha256").hexdigest()
    return dict(target=TARGETS[component], tag=tag, file=path.name, code=sequence,
                version=label, size=length, sha256=digest, force=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--key", type=Path, required=True)
    parser.add_argument("--openssl", default="openssl")
    parser.add_argument("--previous", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--notes", help="Release notes shown in the clients, up to 512 UTF-8 bytes")
    for component in TARGETS:
        parser.add_argument(f"--{component}", nargs=4, metavar=("FILE", "TAG", "CODE", "VERSION"))
        parser.add_argument(f"--{component}-mode", choices=("normal", "force", "withdraw"))
    args = parser.parse_args()
    try:
        data = load_previous(args.openssl, args.previous) if args.previous else {"schema": 1}
        for component in TARGETS:
            item, mode = getattr(args, component), getattr(args, component + "_mode")
            if mode == "withdraw":
                if item:
                    raise ValueError("withdrawal cannot also supply an asset")
                data.pop(component, None)
                continue
            if item:
                next_asset = asset(component, item)
                prior = data.get(component)
                if prior and (next_asset["code"] < prior["code"] or
                              next_asset["code"] == prior["code"] and next_asset["sha256"] != prior["sha256"]):
                    raise ValueError("advance the version when binary contents change")
                data[component] = next_asset
            if mode:
                if component not in data:
                    raise ValueError(f"no {component} asset to set mode")
                data[component]["force"] = mode == "force"
        if args.notes is not None:
            if len(args.notes.encode()) > 512 or any(ord(c) < 32 and c not in "\t\n" for c in args.notes):
                raise ValueError("invalid release notes")
            data["notes"] = args.notes
        payload = json.dumps(data, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode()
        if len(payload) > 4096:
            raise ValueError("manifest payload is too large")
        signature = command(args.openssl, "dgst", "-sha256", "-sign", args.key, input=payload,
                            stdout=subprocess.PIPE).stdout
        with tempfile.TemporaryDirectory() as folder:
            sig = Path(folder) / "signature"
            sig.write_bytes(signature)
            command(args.openssl, "dgst", "-sha256", "-verify", PUBLIC_KEY, "-signature", sig,
                    input=payload, stdout=subprocess.PIPE)
        envelope = {k: base64.b64encode(v).decode() for k, v in {"payload": payload, "signature": signature}.items()}
        with args.output.open("x", encoding="ascii", newline="\n") as out:
            out.write(json.dumps(envelope, separators=(",", ":")) + "\n")
    except (ValueError, KeyError, TypeError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(2, f"{exc}\n")


if __name__ == "__main__":
    main()
