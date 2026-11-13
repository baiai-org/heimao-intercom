# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline regression of real parsing, erase/write plans, workers and concurrency."""
import contextlib
import io
import json
import os
from pathlib import Path
import queue
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import app as flasher_app

from firmware import (APP_OFFSET, DEFAULT_BIN, FLASH_SIZE, Firmware, PARTITIONS,
                      digest, flash_steps)
from runner import Batch
from worker import perform

REPO = Path(__file__).resolve().parents[1]
FIXTURE = Path(os.environ.get('ORL_FLASHER_FIXTURE', str(REPO / '.tmp/portable-flasher/stage/assets' / DEFAULT_BIN)))
FIXTURE_AVAILABLE = FIXTURE.exists()
APP_BIN = Path(os.environ.get('HM_FLASHER_APP', str(REPO / 'firmware/build/openradiolink.bin')))
SKIP_FIXTURE = 'set ORL_FLASHER_FIXTURE to a full merged BIN'


class ExternalFirmwareTests(unittest.TestCase):
    def test_ignores_internal_firmware(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);home=root/'home';assets=root/'resources/assets'
            home.mkdir();assets.mkdir(parents=True);(assets/DEFAULT_BIN).write_bytes(b'stale')
            with patch.object(flasher_app,'home_directory',return_value=home), patch.object(flasher_app,'resource_root',return_value=assets.parent):
                self.assertIsNone(flasher_app.default_firmware())
                self.assertEqual(len(flasher_app.bundled_firmware_files()),1)

    def test_external_image_does_not_hide_bundled_inventory(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);home=root/'home';assets=root/'resources/assets'
            home.mkdir();assets.mkdir(parents=True)
            external=home/DEFAULT_BIN;external.write_bytes(b'external')
            (assets/'older.bin').write_bytes(b'internal')
            with patch.object(flasher_app,'home_directory',return_value=home), patch.object(flasher_app,'resource_root',return_value=assets.parent):
                self.assertEqual(flasher_app.default_firmware(),external)
                self.assertEqual(Path(flasher_app.bundled_firmware_files()[0]).name,'older.bin')

    def test_missing_or_directory_is_not_a_firmware_file(self):
        with tempfile.TemporaryDirectory() as temp:
            home=Path(temp);(home/DEFAULT_BIN).mkdir()
            with patch.object(flasher_app,'home_directory',return_value=home), patch.object(flasher_app,'resource_root',return_value=home/'missing'):
                self.assertIsNone(flasher_app.default_firmware())
                self.assertEqual(flasher_app.bundled_firmware_files(),[])


class ParserTests(unittest.TestCase):
    """Fixture-free checks of the real parser and flash-plan code."""

    def test_rejects_ota_image(self):
        data = bytearray(APP_OFFSET + 0xD0 + 16)
        data[0x20:0x24] = (0xABCD5432).to_bytes(4, 'little')
        with self.assertRaises(ValueError) as caught:
            Firmware.parse(bytes(data))
        self.assertIn('OTA', str(caught.exception))

    def test_rejects_truncated_image(self):
        with self.assertRaises(ValueError):
            Firmware.parse(b'\xE9' * 100)

    def test_flash_plan_rejects_bad_inputs(self):
        with self.assertRaises(ValueError):
            flash_steps('keep', '/dev/ttyUSB0', 460800, [], 'table.bin')
        with self.assertRaises(ValueError):
            flash_steps('unknown', 'COM5', 460800, [], 'table.bin')
        with self.assertRaises(ValueError):
            flash_steps('keep', 'COM5', 12345, [], 'table.bin')


@unittest.skipUnless(FIXTURE_AVAILABLE, SKIP_FIXTURE)
class FirmwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (REPO / '.tmp').mkdir(exist_ok=True)
        cls.firmware = Firmware.load(FIXTURE)

    @unittest.skipUnless(APP_BIN.exists(), 'set HM_FLASHER_APP to the matching application BIN')
    def test_pinned_app_is_byte_identical(self):
        app = APP_BIN.read_bytes()
        self.assertEqual(self.firmware.version,app[0x30:0x50].split(b'\0')[0].decode())
        self.assertEqual(self.firmware.app_sha256,digest(app))
        self.assertEqual(self.firmware.elf_sha256,app[0xb0:0xd0].hex())

    def test_reject_corruption_and_wrong_images(self):
        for offset in (0x100, 0x8001, 0x9000, 0xF000, 0x11000, APP_OFFSET + 1000):
            with self.subTest(offset=hex(offset)):
                bad = bytearray(self.firmware.data)
                bad[offset] ^= 0x01
                with self.assertRaises(ValueError):
                    Firmware.parse(bytes(bad))
        for data in (self.firmware.data[APP_OFFSET:], self.firmware.data[:-100],
                     self.firmware.data + b'\xFF', b'\xFF' * FLASH_SIZE):
            with self.assertRaises(ValueError):
                Firmware.parse(data)

    def test_preserve_vs_factory_flash_sector_effects(self):
        for mode in ('keep', 'factory'):
            with self.subTest(mode=mode):
                with tempfile.TemporaryDirectory(dir=REPO / '.tmp') as temp:
                    pairs = self.firmware.extract(temp)
                    plan = flash_steps(mode, 'COM12', 460800, pairs, Path(temp) / 'table.bin')
                operations = [args for _, args in plan]
                self.assertEqual(any('erase_flash' in args for args in operations), mode == 'factory')
                self.assertNotIn('--force', str(operations))
                self.assertIn('--verify', operations[-1])
                self.assertEqual('read_flash' in operations[1], mode == 'keep')
                # Model the REAL 4 KB erase granularity, not just byte writes.
                flash = bytearray(b'\x35' * FLASH_SIZE)
                if mode == 'factory':
                    flash[:] = b'\xFF' * FLASH_SIZE
                for segment in self.firmware.segments:
                    first = segment.offset // 4096 * 4096
                    end = (segment.offset + len(segment.data) + 4095) // 4096 * 4096
                    flash[first:end] = b'\xFF' * (end - first)
                    flash[segment.offset:segment.offset + len(segment.data)] = segment.data
                for name, _, _, offset, size, _ in PARTITIONS:
                    if name in ('nvs', 'phy_init', 'storage', 'ota_1', 'coredump'):
                        self.assertEqual(flash[offset:offset + size], (b'\x35' if mode == 'keep' else b'\xFF') * size)
                self.assertEqual(flash[0xF000:0x11000], b'\xFF' * 0x2000)
                self.assertEqual(flash[APP_OFFSET:APP_OFFSET + len(self.firmware.segments[-1].data)], self.firmware.segments[-1].data)

    def run_fake(self, mode='keep', chip='ESP32-S3', size='16MB', old_table=None, fail_write=False):
        calls, events = [], []
        with tempfile.TemporaryDirectory(dir=REPO / '.tmp') as temp:
            def invoke(args):
                calls.append(args)
                if 'flash_id' in args:
                    print('Chip is ' + chip)
                    print('Detected flash size: ' + size)
                elif 'read_flash' in args:
                    Path(args[-1]).write_bytes(self.firmware.data[0x8000:0x9000] if old_table is None else old_table)
                elif 'write_flash' in args:
                    if fail_write:
                        raise RuntimeError('simulated verification failure')
                    print('Writing at 0x00020000... (50 %)')
                    print('Verifying 0x100 bytes @ 0x00020000 in flash...')
            request = {'firmware': str(FIXTURE), 'sha256': self.firmware.sha256,
                       'directory': temp, 'mode': mode, 'port': 'COM12', 'baud': 460800}
            try:
                perform(request, lambda kind, **values: events.append({'kind': kind, **values}), invoke)
            except Exception as exc:
                return calls, events, exc
        return calls, events, None

    def test_worker_modes_execute_exact_plans(self):
        for mode in ('keep', 'factory'):
            calls, events, error = self.run_fake(mode)
            self.assertIsNone(error)
            self.assertTrue(events[-1]['ok'])
            self.assertEqual(events[-1]['progress'], 100)
            self.assertEqual(any('erase_flash' in call for call in calls), mode == 'factory')
            self.assertEqual(any('read_flash' in call for call in calls), mode == 'keep')

    def test_preflight_failure_never_erases_or_writes(self):
        for kwargs in ({'chip': 'ESP32'}, {'size': '4MB'}, {'old_table': b'\xFF' * 4096}):
            with self.subTest(kwargs=kwargs):
                calls, events, error = self.run_fake(**kwargs)
                self.assertIsNotNone(error)
                self.assertFalse(any('erase_flash' in call or 'write_flash' in call for call in calls))
                self.assertFalse(any(event.get('ok') for event in events))

    def test_verification_failure_is_not_success(self):
        _, events, error = self.run_fake(fail_write=True)
        self.assertIsNotNone(error)
        self.assertFalse(any(event.get('ok') for event in events))


FAKE_WORKER = r'''
import hashlib,json,pathlib,sys,time
request=json.loads(pathlib.Path(sys.argv[-1]).read_text(encoding='utf-8'))
root=pathlib.Path(request['directory']).parent
events=pathlib.Path(request['events']).open('w',encoding='utf-8',buffering=1)
def emit(**values):
    events.write(json.dumps(values,ensure_ascii=False)+'\n'); events.flush()
assert hashlib.sha256(pathlib.Path(request['firmware']).read_bytes()).hexdigest()==request['sha256']
started=time.time()
emit(kind='log',text='START '+str(started)+' 中文')
if (root/'sleep').exists(): time.sleep(30)
else: time.sleep(.8)
ok=request['port']!='COM2'
emit(kind='log',text='END '+str(time.time()))
emit(kind='result',ok=ok,text='成功' if ok else '模拟失败',progress=100 if ok else 0)
events.close()
sys.exit(0 if ok else 1)
'''


@unittest.skipUnless(FIXTURE_AVAILABLE, SKIP_FIXTURE)
class ParallelTests(unittest.TestCase):
    def setUp(self):
        (REPO / '.tmp').mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=REPO / '.tmp', prefix='中文 路径-')
        self.directory = Path(self.temp.name)
        self.script = self.directory / '模拟 工作进程.py'
        self.script.write_text(FAKE_WORKER, encoding='utf-8')
        self.firmware = Firmware.load(FIXTURE)

    def tearDown(self):
        self.temp.cleanup()

    def make(self, ports):
        return Batch(self.firmware, ports, 'keep', 460800, [sys.executable, str(self.script)])

    def drain(self, batch):
        events = []
        while True:
            try:
                events.append(batch.events.get_nowait())
            except queue.Empty:
                return events

    def test_genuine_concurrency_failure_isolation_and_unicode_paths(self):
        batch = self.make(['COM1', 'COM2', 'COM3'])
        batch.start()
        self.assertTrue(batch.finished.wait(12))
        events = self.drain(batch)
        results = {port: event for port, event in events if event['kind'] == 'result'}
        self.assertTrue(results['COM1']['ok'] and results['COM3']['ok'])
        self.assertFalse(results['COM2']['ok'])
        starts = [float(event['text'].split()[1]) for _, event in events
                  if event['kind'] == 'log' and event['text'].startswith('START')]
        ends = [float(event['text'].split()[1]) for _, event in events
                if event['kind'] == 'log' and event['text'].startswith('END')]
        self.assertEqual(len(starts), 3)
        self.assertLess(max(starts), min(ends), 'Workers ran sequentially')
        self.assertFalse(batch.processes)

    def test_cancel_releases_all_workers(self):
        batch = self.make(['COM1', 'COM3'])
        (batch.root / 'sleep').touch()
        batch.start()
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if len(batch.processes) == 2:
                break
            time.sleep(.02)
        batch.cancel()
        self.assertTrue(batch.finished.wait(10))
        results = [event for _, event in self.drain(batch) if event['kind'] == 'result']
        self.assertEqual(len(results), 2)
        self.assertFalse(any(result.get('ok') for result in results))
        self.assertFalse(batch.processes)

    def test_cancel_before_start_has_no_escaped_process(self):
        batch = self.make(['COM1', 'COM3'])
        batch.cancel()
        batch.start()
        self.assertTrue(batch.finished.wait(5))
        self.assertFalse(batch.processes)

    def test_port_directory_avoids_windows_device_names(self):
        """COM1..COM9 are reserved: a bare .../COM3 path breaks on Win10."""
        self.assertEqual(str(Batch.port_directory('C:/tmp', 'COM1')), 'C:\\tmp\\port-COM1')
        self.assertEqual(str(Batch.port_directory('C:/tmp', 'COM3')), 'C:\\tmp\\port-COM3')
        self.assertEqual(str(Batch.port_directory('C:/tmp', 'COM12')), 'C:\\tmp\\port-COM12')

    def test_reject_duplicate_and_pathlike_ports(self):
        for ports in ([], ['COM1', 'COM1'], ['../bad'], ['COM3/../bad']):
            with self.assertRaises(ValueError):
                self.make(ports)


if __name__ == '__main__':
    unittest.main(verbosity=2)
