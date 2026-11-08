# SPDX-License-Identifier: GPL-3.0-or-later
"""Each COM runs in its own process: esptool globals/stdout stay isolated."""
import contextlib
import io
import json
from pathlib import Path
import re
import time
import traceback

from firmware import Firmware, PARTITIONS, flash_steps, partition_layout


class EventWriter:
    def __init__(self, path):
        self.file = Path(path).open('w', encoding='utf-8', buffering=1)

    def __call__(self, kind, **values):
        self.file.write(json.dumps({'kind': kind, 'time': time.time(), **values}, ensure_ascii=False) + '\n')
        self.file.flush()


class EsptoolOutput(io.TextIOBase):
    def __init__(self, emit, firmware):
        self.emit, self.firmware = emit, firmware
        self.pending = ''
        self.stage = ''
        self.capture = []
        self.progress = 0

    @property
    def encoding(self):
        return 'utf-8'

    def isatty(self):
        return False

    def write(self, text):
        self.pending += text.replace('\x08', '')
        parts = re.split('[\r\n]', self.pending)
        self.pending = parts.pop()
        for line in parts:
            if line.strip():
                self.line(line.strip())
        return len(text)

    def flush(self):
        if self.pending.strip():
            self.line(self.pending.strip())
            self.pending = ''

    def line(self, line):
        self.capture.append(line)
        self.emit('log', text=line)
        if self.stage == 'write':
            match = re.search(r'Writing at 0x([0-9a-fA-F]+).*?\(\s*(\d+)\s*%\)', line)
            if match:
                address, percent = int(match[1], 16), min(100, int(match[2]))
                total = sum(len(part.data) for part in self.firmware.segments)
                done = 0
                for part in self.firmware.segments:
                    if part.offset <= address < part.offset + len(part.data) + 0x1000:
                        done += len(part.data) * percent / 100
                        break
                    done += len(part.data)
                self.progress = max(self.progress, min(92, 15 + int(77 * done / total)))
                self.emit('status', text='正在写入', progress=self.progress)
            elif line.startswith('Verifying '):
                self.emit('status', text='正在校验', progress=95)


def perform(request, emit, invoke=None):
    """Inject only the esptool call in tests; production always uses esptool.main."""
    import esptool
    invoke = invoke or esptool.main
    firmware = Firmware.load(request['firmware'])
    if firmware.sha256 != request['sha256']:
        raise ValueError('固件在任务启动后发生变化，已拒绝刷写。')
    directory = Path(request['directory'])
    pairs = firmware.extract(directory)
    table = directory / 'target-partition-table.bin'
    output = EsptoolOutput(emit, firmware)
    for stage, args in flash_steps(request['mode'], request['port'], request['baud'], pairs, table):
        output.stage = stage
        output.capture.clear()
        labels = {'probe': ('正在连接并检查芯片', 1), 'layout': ('检查原分区布局', 5),
                  'erase': ('正在全片擦除', 5), 'write': ('正在写入', 15)}
        text, progress = labels[stage]
        emit('status', text=text, progress=progress)
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            result = invoke(args)
            output.flush()
        if result not in (None, 0):
            raise RuntimeError('esptool 返回失败：' + str(result))
        if stage == 'probe':
            captured = '\n'.join(output.capture)
            if not re.search(r'Chip is ESP32-S3\b', captured):
                raise ValueError('芯片不是 ESP32-S3，已停止，未写入固件。')
            if not re.search(r'Detected flash size:\s*16MB\b', captured):
                raise ValueError('此固件需要 16 MB Flash，检测不符，已停止。')
        elif stage == 'layout':
            if partition_layout(table.read_bytes()) != PARTITIONS:
                raise ValueError('现有分区布局不同；保留配置模式已停止，请选择全新刷机。')
    emit('result', ok=True, text='成功（写入并校验通过）', progress=100,
         version=firmware.version, sha256=firmware.sha256)


def run_worker(request_path):
    request = json.loads(Path(request_path).read_text(encoding='utf-8'))
    emit = EventWriter(request['events'])
    try:
        perform(request, emit)
        return 0
    except (Exception, SystemExit) as exc:
        emit('log', text=traceback.format_exc())
        emit('result', ok=False, text=str(exc) or 'esptool 未正常完成', progress=0)
        return 1
    finally:
        emit.file.close()
