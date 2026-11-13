# SPDX-License-Identifier: GPL-3.0-or-later
"""Inspect the actual PyInstaller EXE; never opens a serial port."""
import argparse
import hashlib
import json
from pathlib import Path
from PyInstaller.archive.readers import CArchiveReader


def inspect_bundle(exe, forbidden_images=()):
    archive = CArchiveReader(str(exe))
    names = [name.replace('\\', '/') for name in archive.toc]
    assets = sorted(name for name in names if name.startswith('assets/'))
    expected = ['assets/brand-icon.png', 'assets/heimao.ico']
    if assets != expected:
        raise ValueError('Unexpected bundle assets: '+repr(assets))
    firmware_names = [name for name in names if name.lower().endswith('.bin') and
                      (name.startswith('assets/') or 'heimao' in name.lower() or 'openradiolink' in name.lower())]
    if firmware_names:
        raise ValueError('Handset firmware found in bundle: '+repr(firmware_names))
    hashes = {}
    for image in forbidden_images:
        image = Path(image)
        hashes.setdefault(image.stat().st_size, set()).add(hashlib.sha256(image.read_bytes()).hexdigest())
    for name, entry in archive.toc.items():
        if entry[2] in hashes and hashlib.sha256(archive.extract(name)).hexdigest() in hashes[entry[2]]:
            raise ValueError('External handset image was packed as '+name)
    return {'exe_sha256': hashlib.sha256(Path(exe).read_bytes()).hexdigest(),
            'archive_entries': len(names), 'assets': assets,
            'embedded_firmware': False, 'bundled_firmware_files': [],
            'external_images_checked': len(forbidden_images), 'serial_opened': False, 'flash_requested': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--forbidden-image', type=Path, action='append', default=[])
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    result = inspect_bundle(args.exe, args.forbidden_image)
    args.report.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
