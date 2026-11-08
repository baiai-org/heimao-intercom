#!/usr/bin/env python3
"""Generate the self-signed HTTPS certificate used by the local panel.

The firmware embeds ``servercert.pem`` / ``serverkey.pem`` into the image.  The
private key must never be committed: it is generated per build (or per board
provisioning step) and is ignored by git.  ``firmware/main/CMakeLists.txt``
runs this script automatically at configure time when either file is missing.

Requires the ``cryptography`` package, which ships with the ESP-IDF Python
environment (esptool depends on it).

    python tools/gen_dev_cert.py --out firmware/main --cn orl.local --days 3650
"""
import argparse
import datetime
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID


def generate(out, cn, days, force):
    cert_path = Path(out) / "servercert.pem"
    key_path = Path(out) / "serverkey.pem"
    if cert_path.exists() and key_path.exists() and not force:
        print(f"certificate already present: {cert_path}")
        return 0

    key = ec.generate_private_key(ec.SECP256R1())
    now = datetime.datetime.now(datetime.timezone.utc)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])
    certificate = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=days))
        .add_extension(
            x509.SubjectAlternativeName([x509.DNSName(cn)]),
            critical=False,
        )
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .sign(key, hashes.SHA256())
    )

    key_path.write_bytes(
        key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.PKCS8,
            encryption_algorithm=serialization.NoEncryption(),
        )
    )
    cert_path.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
    print(f"generated {cert_path.name} / {key_path.name} (CN={cn}, {days} days)")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default="firmware/main", help="directory for the PEM files")
    parser.add_argument("--cn", default="orl.local", help="certificate common name")
    parser.add_argument("--days", type=int, default=3650, help="validity in days")
    parser.add_argument("--force", action="store_true", help="overwrite existing files")
    args = parser.parse_args()
    return generate(args.out, args.cn, args.days, args.force)


if __name__ == "__main__":
    raise SystemExit(main())
