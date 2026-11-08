# SPDX-License-Identifier: GPL-3.0-or-later
"""Parallel worker lifecycle, cancellation, immutable firmware, queue-only UI."""
import json
from pathlib import Path
import queue
import re
import subprocess
import tempfile
import threading
import time


class Batch:
    def __init__(self, firmware, ports, mode, baud, command):
        if not ports or len(set(ports)) != len(ports):
            raise ValueError('请选择不重复的串口。')
        if any(not re.fullmatch(r'COM\d+', port, re.I) for port in ports):
            raise ValueError('无效的 COM 口。')
        self.firmware, self.ports = firmware, tuple(ports)
        self.mode, self.baud, self.command = mode, int(baud), tuple(command)
        self.events = queue.Queue()
        self.cancelled = threading.Event()
        self.finished = threading.Event()
        self.processes = {}
        self.lock = threading.Lock()
        self.temp = tempfile.TemporaryDirectory(prefix='Heimao-Flasher-')
        self.root = Path(self.temp.name)
        self.snapshot = self.root / 'firmware.bin'
        self.snapshot.write_bytes(firmware.data)

    @staticmethod
    def port_directory(root, port):
        """A per-port directory that is never a Windows device name.

        COM1..COM9 are reserved device names.  A bare "...\\COM3" directory is
        accepted by some Windows builds but later opens/writes under it can
        fail with WinError 267 ("The directory name is invalid") -- seen on a
        second Win10 PC.  The prefix keeps it an ordinary file path.
        """
        return Path(root) / ('port-' + port)

    def start(self):
        threading.Thread(target=self._all, daemon=True).start()

    def _all(self):
        threads = [threading.Thread(target=self._one, args=(port,), daemon=True) for port in self.ports]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.finished.set()
        self.events.put(('', {'kind': 'batch_done'}))
        # Workers are exited and COM handles released before deleting their data.
        self.temp.cleanup()

    def _one(self, port):
        last_result = None
        process = None
        reader = None
        try:
            directory = self.port_directory(self.root, port)
            directory.mkdir()
            events = directory / 'events.jsonl'
            request = {'port': port, 'firmware': str(self.snapshot), 'sha256': self.firmware.sha256,
                       'mode': self.mode, 'baud': self.baud, 'directory': str(directory), 'events': str(events)}
            path = directory / 'request.json'
            path.write_text(json.dumps(request), encoding='utf-8')
            flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
            # Register and launch atomically with cancel, preventing an escaped child.
            with self.lock:
                if self.cancelled.is_set():
                    raise InterruptedError('已取消，未开始刷写')
                process = subprocess.Popen([*self.command, '--worker', str(path)],
                                           creationflags=flags, stdin=subprocess.DEVNULL,
                                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                self.processes[port] = process
            pending = b''
            while True:
                if self.cancelled.is_set() and process.poll() is None:
                    process.terminate()
                if reader is None and events.exists():
                    reader = events.open('rb')
                if reader is not None:
                    pending += reader.read()
                    while b'\n' in pending:
                        line, pending = pending.split(b'\n', 1)
                        if line:
                            event = json.loads(line)
                            if event['kind'] == 'result':
                                last_result = event
                            else:
                                self.events.put((port, event))
                if process.poll() is not None:
                    # One final drain after exit (all writes have now been flushed).
                    if reader is None and events.exists():
                        reader = events.open('rb')
                    if reader is not None:
                        for line in (pending + reader.read()).splitlines():
                            event = json.loads(line)
                            if event['kind'] == 'result':
                                last_result = event
                            else:
                                self.events.put((port, event))
                    break
                time.sleep(0.08)
            if last_result and process.returncode == 0 and last_result.get('ok'):
                self.events.put((port, last_result))
            elif self.cancelled.is_set():
                raise InterruptedError('已取消；若已开始写入，请重新刷机')
            else:
                raise RuntimeError((last_result or {}).get('text', f'刷写进程异常退出（{process.returncode}）'))
        except Exception as exc:
            self.events.put((port, {'kind': 'result', 'ok': False, 'text': str(exc), 'progress': 0}))
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
            if reader is not None:
                reader.close()
            with self.lock:
                self.processes.pop(port, None)

    def cancel(self):
        self.cancelled.set()
        with self.lock:
            for process in self.processes.values():
                if process.poll() is None:
                    try:
                        process.terminate()
                    except OSError:
                        pass
