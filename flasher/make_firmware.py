# SPDX-License-Identifier: GPL-3.0-or-later
"""Merge the current built app, bootloader, partition table and OTA data."""
import argparse
import json
from pathlib import Path

import esptool

from firmware import DEFAULT_BIN, Firmware, PARTITIONS, digest, partition_layout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    repo, output = args.repo.resolve(), args.output.resolve()
    build = repo / 'firmware/build'
    app=build/'openradiolink.bin'
    raw=app.read_bytes()
    accepted={'bin_sha256':digest(raw),'elf_sha256':raw[0xb0:0xd0].hex(),
              'source_commit':'see firmware version and release BUILD-INFO.json'}
    table = build / 'partition_table/partition-table.bin'
    if partition_layout(table.read_bytes()) != PARTITIONS:
        raise ValueError('Build partition table differs from this board')
    parts = ((0, build / 'bootloader/bootloader.bin'), (0x8000, table),
             (0xF000, build / 'ota_data_initial.bin'), (0x20000, app))
    output.mkdir(parents=True, exist_ok=True)
    target = output / DEFAULT_BIN
    command = ['--chip', 'esp32s3', 'merge_bin', '-o', str(target),
               '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size', '16MB']
    for address, path in parts:
        command.extend((hex(address), str(path)))
    esptool.main(command)
    firmware = Firmware.load(target)
    if firmware.app_sha256 != accepted['bin_sha256'] or firmware.elf_sha256 != accepted['elf_sha256']:
        raise ValueError('Merging changed the accepted application')
    manifest = {'file': DEFAULT_BIN, 'format': 'standard raw merged BIN at offset 0x0',
                'version': firmware.version, 'bytes': len(firmware.data), 'sha256': firmware.sha256,
                'app_sha256': firmware.app_sha256, 'elf_sha256': firmware.elf_sha256,
                'accepted_source_commit': accepted['source_commit'], 'application_rebuilt': False,
                'segments': [{'name': part.name, 'offset': hex(part.offset), 'bytes': len(part.data),
                              'sha256': digest(part.data)} for part in firmware.segments],
                'keep_mode': 'split segments; preserve NVS, PHY, storage, other OTA and coredump; reset OTA selection to ota_0',
                'factory_mode': 'erase entire flash, then write and verify all firmware segments'}
    (output / 'firmware-manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(manifest, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
