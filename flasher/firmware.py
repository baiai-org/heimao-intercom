# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate a standard merged ESP32-S3 BIN and select safe write ranges."""
from dataclasses import dataclass
import hashlib
import io
from pathlib import Path
import struct

from esptool.bin_image import LoadFirmwareImage
from brand import DEFAULT_BIN, DEVICE

FLASH_SIZE = 16 * 1024 * 1024
APP_OFFSET = 0x20000
APP_LIMIT = 0x420000
# Exact layout of this board. Preserve mode rejects a different target layout.
PARTITIONS = (
    ('nvs', 1, 2, 0x9000, 0x6000, 0),
    ('otadata', 1, 0, 0xF000, 0x2000, 0),
    ('phy_init', 1, 1, 0x11000, 0x1000, 0),
    ('ota_0', 0, 16, 0x20000, 0x400000, 0),
    ('ota_1', 0, 17, 0x420000, 0x400000, 0),
    ('storage', 1, 130, 0x820000, 0x100000, 0),
    ('coredump', 1, 3, 0x920000, 0x40000, 0),
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def partition_layout(data):
    if len(data) < 0xC00:
        raise ValueError('分区表不完整。')
    entries = []
    for pos in range(0, 0xC00, 32):
        entry = data[pos:pos + 32]
        if entry[:16] == b'\xEB\xEB' + b'\xFF' * 14:
            if entry[16:] != hashlib.md5(data[:pos]).digest():
                raise ValueError('分区表 MD5 校验失败。')
            if data[pos + 32:] != b'\xFF' * (len(data) - pos - 32):
                raise ValueError('分区表包含不支持的额外数据。')
            return tuple(entries)
        magic, kind, subtype, offset, size, label, flags = struct.unpack('<HBBII16sI', entry)
        if magic != 0x50AA:
            raise ValueError('未找到有效的分区表；请选择本手台的完整合并 BIN。')
        entries.append((label.split(b'\0')[0].decode('ascii'), kind, subtype, offset, size, flags))
    raise ValueError('分区表缺少 MD5 校验项。')


def image_length(data, offset, limit):
    try:
        image = LoadFirmwareImage('esp32s3', io.BytesIO(data[offset:limit]))
        if image.chip_id != 9:
            raise ValueError('固件不是 ESP32-S3 镜像。')
        if image.checksum != image.calculate_checksum():
            raise ValueError('固件段校验失败。')
        if not image.append_digest or image.stored_digest != image.calc_digest:
            raise ValueError('固件 SHA256 校验失败。')
        size = image.data_length + 32
        if offset + size > min(limit, len(data)):
            raise ValueError('固件镜像不完整。')
        return size
    except ValueError:
        raise
    except Exception as exc:
        raise ValueError('无法解析 ESP32-S3 镜像：' + str(exc)) from exc


@dataclass(frozen=True)
class Segment:
    name: str
    offset: int
    data: bytes


@dataclass(frozen=True)
class Firmware:
    data: bytes
    version: str
    sha256: str
    app_sha256: str
    elf_sha256: str
    segments: tuple

    @classmethod
    def load(cls, path):
        path = Path(path)
        if path.stat().st_size > APP_LIMIT:
            raise ValueError('文件过大；请选择本手台的完整合并 BIN，而非整片 Flash 备份。')
        return cls.parse(path.read_bytes())

    @classmethod
    def parse(cls, data):
        if not (APP_OFFSET + 0xD0 <= len(data) <= APP_LIMIT):
            raise ValueError('BIN 大小不正确，请选择完整合并固件。')
        if struct.unpack_from('<I', data, 0x20)[0] == 0xABCD5432:
            raise ValueError('这是 OTA 应用 BIN，请选择文件名带 full 的完整合并 BIN。')
        if partition_layout(data[0x8000:0x9000]) != PARTITIONS:
            raise ValueError('固件分区布局与此手台不匹配，已拒绝刷写。')
        boot_size = image_length(data, 0, 0x8000)
        app_size = image_length(data, APP_OFFSET, APP_LIMIT)
        if data[2:4] != bytes((2, 0x4F)):
            raise ValueError('此手台需要 DIO / 80 MHz / 16 MB 固件。')
        if APP_OFFSET + app_size != len(data):
            raise ValueError('BIN 含有无法识别的尾部数据。')
        app = data[APP_OFFSET:]
        if struct.unpack_from('<I', app, 0x20)[0] != 0xABCD5432:
            raise ValueError('固件缺少 ESP-IDF 应用描述。')
        project = app[0x50:0x70].split(b'\0')[0].decode('utf-8')
        version = app[0x30:0x50].split(b'\0')[0].decode('utf-8')
        if project != 'openradiolink' or not version:
            raise ValueError(f'请选择{DEVICE}的固件。')
        # Keep the delivery BIN standard/raw; discard all padding when writing.
        # In particular NEVER write its FF-filled NVS or PHY gaps in keep mode.
        for start, end in ((boot_size, 0x8000), (0x8C00, 0xF000), (0x11000, APP_OFFSET)):
            if data[start:end] != b'\xFF' * (end - start):
                raise ValueError('合并固件包含额外配置数据，已拒绝刷写。')
        if data[0xF000:0x11000] != b'\xFF' * 0x2000:
            raise ValueError('OTA 初始化数据无效，无法保证启动到新固件。')
        segments = (Segment('bootloader', 0, data[:boot_size]),
                    Segment('partitions', 0x8000, data[0x8000:0x8C00]),
                    Segment('otadata', 0xF000, data[0xF000:0x11000]),
                    Segment('application', APP_OFFSET, app))
        return cls(bytes(data), version, digest(data), digest(app), app[0xB0:0xD0].hex(), segments)

    def extract(self, directory):
        pairs = []
        for segment in self.segments:
            path = Path(directory) / (segment.name + '.bin')
            path.write_bytes(segment.data)
            pairs.extend((hex(segment.offset), str(path)))
        return pairs


def flash_steps(mode, port, baud, pairs, table_path):
    """One authoritative plan shared by workers and offline tests."""
    if mode not in ('keep', 'factory'):
        raise ValueError('未知刷机模式。')
    if not port or not port.upper().startswith('COM') or not port[3:].isdigit():
        raise ValueError('无效的 COM 口。')
    if int(baud) not in (115200, 460800, 921600):
        raise ValueError('不支持的波特率。')
    base = ['--chip', 'esp32s3', '--port', port, '--baud', str(baud),
            '--before', 'default_reset', '--after', 'no_reset']
    steps = [('probe', base + ['flash_id'])]
    if mode == 'keep':
        steps.append(('layout', base + ['read_flash', '0x8000', '0x1000', str(table_path)]))
    else:
        steps.append(('erase', base + ['erase_flash']))
    final = base[:-1] + ['hard_reset', 'write_flash', '--verify',
                        '--flash_mode', 'keep', '--flash_freq', 'keep', '--flash_size', 'keep']
    steps.append(('write', final + list(pairs)))
    return steps
