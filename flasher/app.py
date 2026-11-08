# SPDX-License-Identifier: GPL-3.0-or-later
"""黑猫互联 portable GUI, adapted from arnaudlvq/ESP32-Flasher.

Upstream flasher_tk.py at 536704a2c33d4f913da5eb2ddfa56424c9667722.
Modifications: Chinese UI, S3/16MB single-BIN validation, two flash modes,
multi-select ports, isolated concurrent workers, progress and cancellation.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import queue
import sys
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import serial.tools.list_ports

from firmware import DEFAULT_BIN, Firmware
from runner import Batch
from worker import run_worker
from brand import PROJECT, DEVICE, TOOL, VERSION



def resource_root():
    return Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parent))


def home_directory():
    return Path(sys.executable).parent if getattr(sys, 'frozen', False) else Path(__file__).resolve().parent


def worker_command():
    return [sys.executable] if getattr(sys, 'frozen', False) else [sys.executable, str(Path(__file__).resolve())]


def default_firmware():
    external = home_directory() / DEFAULT_BIN
    if external.exists():
        return external
    internal = resource_root() / 'assets' / DEFAULT_BIN
    return internal if internal.exists() else None


class ESPFlasherApp:
    def __init__(self, root):
        self.root = root
        self.root.title(TOOL + ' ' + VERSION)
        icon=resource_root() / 'assets' / 'heimao.ico'
        if icon.exists(): self.root.iconbitmap(str(icon))
        self.root.geometry('1020x730')
        self.root.minsize(900, 660)
        self.firmware = None
        self.batch = None
        self.checked = set()
        self.results = {}
        self.port_info = {}
        self.log_lines = []
        self.busy = False
        self.closing = False
        self.mode = tk.StringVar(value='keep')
        self.baud = tk.StringVar(value='460800')
        self.path_text = tk.StringVar()
        self.firmware_text = tk.StringVar(value='正在读取固件…')
        self.status_text = tk.StringVar(value='连接 USB 数据线，勾选要刷写的 COM 口。')
        style = ttk.Style()
        if 'vista' in style.theme_names():
            style.theme_use('vista')
        style.configure('.', font=('Microsoft YaHei UI', 10))
        style.configure('Treeview', rowheight=31)
        style.configure('Title.TLabel', font=('Microsoft YaHei UI', 16, 'bold'))
        style.configure('Hint.TLabel', foreground='#526175')
        self.create_widgets()
        initial = default_firmware()
        if initial is not None:
            self.load_firmware(initial, initial=True)
        else:
            self.firmware_text.set('请点击“选择 BIN”，选择本手台的完整合并固件。')
            self.path_text.set('未选择固件')
        self.refresh_ports()
        self.root.after(1800, self.auto_refresh)
        self.root.after(70, self.pump_events)
        self.root.protocol('WM_DELETE_WINDOW', self.close)

    def create_widgets(self):
        main_frame = ttk.Frame(self.root, padding=18)
        main_frame.pack(fill=tk.BOTH, expand=True)
        title_row=ttk.Frame(main_frame);title_row.pack(fill=tk.X)
        logo=resource_root() / 'assets' / 'brand-icon.png'
        if logo.exists():
            self.logo=tk.PhotoImage(file=str(logo)).subsample(4,4)
            ttk.Label(title_row,image=self.logo).pack(side=tk.LEFT,padx=(0,12))
        ttk.Label(title_row, text=TOOL, style='Title.TLabel').pack(side=tk.LEFT)
        ttk.Label(main_frame, text=DEVICE+' · ESP32-S3 / 16 MB Flash   |   多串口刷写   |   离线运行',
                  style='Hint.TLabel').pack(anchor=tk.W, pady=(3, 12))

        file_frame = ttk.LabelFrame(main_frame, text='1  选择固件', padding=10)
        file_frame.pack(fill=tk.X)
        row = ttk.Frame(file_frame)
        row.pack(fill=tk.X)
        ttk.Entry(row, textvariable=self.path_text, state='readonly').pack(side=tk.LEFT, fill=tk.X, expand=True)
        self.browse_button = ttk.Button(row, text='选择 BIN…', command=self.browse_file)
        self.browse_button.pack(side=tk.RIGHT, padx=(8, 0))
        ttk.Label(file_frame, textvariable=self.firmware_text, style='Hint.TLabel').pack(anchor=tk.W, pady=(6, 0))

        mode_frame = ttk.LabelFrame(main_frame, text='2  选择刷机方式', padding=10)
        mode_frame.pack(fill=tk.X, pady=10)
        self.keep_radio = ttk.Radiobutton(mode_frame, text='保留配置刷机（推荐）', variable=self.mode,
                                         value='keep', command=self.update_mode)
        self.keep_radio.grid(row=0, column=0, sticky=tk.W, padx=(0, 22))
        self.factory_radio = ttk.Radiobutton(mode_frame, text='全新刷机（清空全部配置）', variable=self.mode,
                                            value='factory', command=self.update_mode)
        self.factory_radio.grid(row=0, column=1, sticky=tk.W)
        self.mode_hint = ttk.Label(mode_frame, text='保留 Wi-Fi、信道、射频/音频参数和已保存的自定义内容。',
                                   style='Hint.TLabel')
        self.mode_hint.grid(row=1, column=0, columnspan=2, sticky=tk.W, pady=(6, 0))

        port_frame = ttk.LabelFrame(main_frame, text='3  勾选要刷写的 COM 口', padding=10)
        port_frame.pack(fill=tk.BOTH, expand=True)
        toolbar = ttk.Frame(port_frame)
        toolbar.pack(fill=tk.X, pady=(0, 7))
        self.refresh_button = ttk.Button(toolbar, text='刷新串口', command=self.refresh_ports)
        self.refresh_button.pack(side=tk.LEFT)
        self.all_button = ttk.Button(toolbar, text='全选', command=self.select_all)
        self.all_button.pack(side=tk.LEFT, padx=6)
        self.none_button = ttk.Button(toolbar, text='清空勾选', command=self.clear_selected)
        self.none_button.pack(side=tk.LEFT)
        ttk.Label(toolbar, text='波特率').pack(side=tk.LEFT, padx=(18, 6))
        self.baud_combo = ttk.Combobox(toolbar, textvariable=self.baud, values=('115200', '460800', '921600'),
                                       state='readonly', width=10)
        self.baud_combo.pack(side=tk.LEFT)
        self.count_label = ttk.Label(toolbar, text='已勾选 0 个', style='Hint.TLabel')
        self.count_label.pack(side=tk.RIGHT)
        table = ttk.Frame(port_frame)
        table.pack(fill=tk.BOTH, expand=True)
        columns = ('check', 'port', 'device', 'progress', 'status')
        self.tree = ttk.Treeview(table, columns=columns, show='headings', selectmode='none', height=5)
        for name, title, width, anchor in (
                ('check', '勾选', 58, tk.CENTER), ('port', '串口', 85, tk.CENTER),
                ('device', '设备', 335, tk.W), ('progress', '进度', 75, tk.CENTER),
                ('status', '状态', 330, tk.W)):
            self.tree.heading(name, text=title)
            self.tree.column(name, width=width, minwidth=width if name != 'device' else 190,
                             stretch=name in ('device', 'status'), anchor=anchor)
        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scroll = ttk.Scrollbar(table, orient=tk.VERTICAL, command=self.tree.yview)
        scroll.pack(side=tk.RIGHT, fill=tk.Y)
        self.tree.configure(yscrollcommand=scroll.set)
        self.tree.tag_configure('success', foreground='#137246')
        self.tree.tag_configure('failed', foreground='#B3261E')
        self.tree.bind('<Button-1>', self.click_port)
        self.tree.bind('<space>', self.space_port)

        action_frame = ttk.Frame(main_frame)
        action_frame.pack(fill=tk.X, pady=10)
        self.flash_button = ttk.Button(action_frame, text='开始刷写已勾选的手台', command=self.flash_esp32)
        self.flash_button.pack(side=tk.LEFT)
        self.stop_button = ttk.Button(action_frame, text='停止刷写', command=self.stop, state=tk.DISABLED)
        self.stop_button.pack(side=tk.LEFT, padx=8)
        self.progress_bar = ttk.Progressbar(action_frame, mode='determinate', maximum=100)
        self.progress_bar.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=8)
        ttk.Button(action_frame, text='保存日志…', command=self.save_log).pack(side=tk.RIGHT)
        ttk.Label(main_frame, textvariable=self.status_text, wraplength=940).pack(anchor=tk.W, pady=(0, 7))
        log_frame = ttk.Frame(main_frame)
        log_frame.pack(fill=tk.X)
        self.log_box = tk.Text(log_frame, height=6, state=tk.DISABLED, wrap=tk.WORD,
                               font=('Microsoft YaHei UI', 9), relief=tk.SOLID, borderwidth=1)
        self.log_box.pack(side=tk.LEFT, fill=tk.X, expand=True)
        log_scroll = ttk.Scrollbar(log_frame, command=self.log_box.yview)
        log_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        self.log_box.configure(yscrollcommand=log_scroll.set)

    def load_firmware(self, path, initial=False):
        try:
            firmware = Firmware.load(path)
        except Exception as exc:
            if not initial:
                messagebox.showerror('固件不可用', str(exc), parent=self.root)
            else:
                self.firmware_text.set('请点击“选择 BIN”，选择本手台的完整合并固件。')
            return
        self.firmware = firmware
        internal = resource_root() / 'assets' / DEFAULT_BIN
        self.path_text.set('内置固件：' + DEFAULT_BIN if Path(path) == internal else str(path))
        self.firmware_text.set(f'固件版本：{firmware.version}   ·   完整 BIN 校验通过   ·   ESP32-S3 / 16 MB')
        self.log(f'已加载固件 {firmware.version}，SHA256 {firmware.sha256}')

    def browse_file(self):
        path = filedialog.askopenfilename(parent=self.root, title='选择'+DEVICE+'完整合并 BIN',
                                          filetypes=[('完整固件 BIN', '*.bin')])
        if path:
            self.load_firmware(path)

    def update_mode(self):
        factory = self.mode.get() == 'factory'
        self.mode_hint.configure(text=('将清空 Wi-Fi、信道、全部参数和自定义内容；刷好后需要重新配置。' if factory
                                      else '保留 Wi-Fi、信道、射频/音频参数和已保存的自定义内容。'),
                                 foreground='#B3261E' if factory else '#526175')

    def refresh_ports(self, automatic=False):
        if self.busy:
            return
        try:
            found = {port.device: port for port in serial.tools.list_ports.comports()}
        except Exception as exc:
            self.status_text.set('扫描串口失败：' + str(exc))
            return
        self.port_info = found
        for port in self.tree.get_children():
            if port not in found:
                self.checked.discard(port)
                if automatic and port in self.results:
                    self.tree.set(port, 'check', '☐')
                    self.tree.set(port, 'device', '设备已断开（保留刷写结果）')
                else:
                    self.tree.delete(port)
        for port in sorted(found, key=lambda name: int(name[3:]) if name[3:].isdigit() else 99999):
            device = found[port]
            description = device.description
            if device.vid == 0x303A and device.pid == 0x1001:
                description = 'ESP32-S3 原生 USB-Serial/JTAG'
            if self.tree.exists(port):
                self.tree.set(port, 'device', description)
            else:
                self.tree.insert('', tk.END, iid=port, values=('☐', port, description, '—', '待选择'))
        self.update_count()
        if not found:
            self.status_text.set('未检测到 COM 口。请连接 USB 数据线，然后点击“刷新串口”。')
        elif not self.results and not automatic:
            self.status_text.set(f'检测到 {len(found)} 个 COM 口；请勾选要刷写的手台。')

    def auto_refresh(self):
        self.refresh_ports(automatic=True)
        self.root.after(1800, self.auto_refresh)

    def toggle(self, port):
        if self.busy or port not in self.port_info:
            return
        self.checked.symmetric_difference_update((port,))
        self.tree.set(port, 'check', '☑' if port in self.checked else '☐')
        self.update_count()

    def click_port(self, event):
        port = self.tree.identify_row(event.y)
        if port:
            self.tree.focus(port)
            self.tree.focus_set()
            self.toggle(port)

    def space_port(self, _event):
        self.toggle(self.tree.focus())
        return 'break'

    def select_all(self):
        if not self.busy:
            self.checked = set(self.port_info)
            for port in self.port_info:
                self.tree.set(port, 'check', '☑')
            self.update_count()

    def clear_selected(self):
        if not self.busy:
            self.checked.clear()
            for port in self.tree.get_children():
                self.tree.set(port, 'check', '☐')
            self.update_count()

    def update_count(self):
        self.count_label.configure(text=f'已勾选 {len(self.checked)} 个')

    def set_busy(self, busy):
        self.busy = busy
        state = tk.DISABLED if busy else tk.NORMAL
        for widget in (self.flash_button, self.refresh_button, self.all_button, self.none_button,
                       self.browse_button, self.keep_radio, self.factory_radio):
            widget.configure(state=state)
        self.baud_combo.configure(state=tk.DISABLED if busy else 'readonly')
        self.stop_button.configure(state=tk.NORMAL if busy else tk.DISABLED)

    def flash_esp32(self):
        if self.busy:
            return
        if self.firmware is None:
            messagebox.showerror('请选择固件', '请先选择有效的完整 BIN 固件。', parent=self.root)
            return
        ports = sorted(self.checked & set(self.port_info))
        if not ports:
            messagebox.showinfo('请选择串口', '请先勾选要刷写的 COM 口。', parent=self.root)
            return
        if self.mode.get() == 'factory' and not messagebox.askyesno(
                '确认全新刷机', f'即将清空以下 {len(ports)} 块手台的全部配置和自定义内容：\n\n'
                + '、'.join(ports) + '\n\n包括 Wi-Fi、信道、参数和自定义尾音。是否继续？', parent=self.root):
            return
        self.results = {}
        self.port_progress = {port: 0 for port in ports}
        for port in ports:
            self.tree.set(port, 'progress', '0%')
            self.tree.set(port, 'status', '准备刷写')
            self.tree.item(port, tags=())
        self.progress_bar['value'] = 0
        try:
            self.batch = Batch(self.firmware, ports, self.mode.get(), int(self.baud.get()), worker_command())
        except Exception as exc:
            messagebox.showerror('无法开始', str(exc), parent=self.root)
            return
        self.set_busy(True)
        self.status_text.set(f'正在同时刷写 {len(ports)} 块手台，请勿拔出 USB 数据线。')
        self.log('开始：' + '、'.join(ports) + ('；全新刷机' if self.mode.get() == 'factory' else '；保留配置刷机'))
        self.batch.start()

    def pump_events(self):
        if self.batch:
            for _ in range(400):
                try:
                    port, event = self.batch.events.get_nowait()
                except queue.Empty:
                    break
                kind = event['kind']
                if kind == 'batch_done':
                    self.set_busy(False)
                    successes = sum(bool(result.get('ok')) for result in self.results.values())
                    self.status_text.set(f'已结束：成功 {successes} 块，未完成 {len(self.results) - successes} 块。'
                                         + (' 全新刷机成功的手台需要重新配网和配置。' if self.mode.get() == 'factory' else ''))
                    if self.closing:
                        self.root.destroy()
                        return
                elif kind == 'log':
                    self.log(event['text'], port)
                elif kind in ('status', 'result'):
                    self.tree.set(port, 'status', event['text'])
                    value = event.get('progress', 0)
                    self.tree.set(port, 'progress', f'{value}%')
                    self.port_progress[port] = value
                    self.progress_bar['value'] = sum(self.port_progress.values()) / len(self.port_progress)
                    if kind == 'result':
                        self.results[port] = event
                        self.tree.item(port, tags=('success' if event.get('ok') else 'failed',))
                        self.log(event['text'], port)
        self.root.after(70, self.pump_events)

    def log(self, line, port=''):
        line = (f'[{port}] ' if port else '') + line
        self.log_lines.append(line)
        self.log_box.configure(state=tk.NORMAL)
        self.log_box.insert(tk.END, line + '\n')
        if int(self.log_box.index('end-1c').split('.')[0]) > 1800:
            self.log_box.delete('1.0', '600.0')
        self.log_box.see(tk.END)
        self.log_box.configure(state=tk.DISABLED)

    def save_log(self):
        path = filedialog.asksaveasfilename(parent=self.root, title='保存刷机日志',
                                        defaultextension='.txt', initialfile=PROJECT+'-刷机日志.txt')
        if path:
            try:
                Path(path).write_text('\n'.join(self.log_lines) + '\n', encoding='utf-8-sig')
            except OSError as exc:
                messagebox.showerror('保存失败', str(exc), parent=self.root)

    def stop(self):
        if self.batch and self.busy:
            self.batch.cancel()
            self.stop_button.configure(state=tk.DISABLED)
            self.status_text.set('正在停止。已经开始擦除或写入的手台可能需要重新刷机。')

    def close(self):
        if self.busy:
            if messagebox.askyesno('仍在刷写', '关闭窗口将中断刷写，相关手台可能需要重新刷机。是否停止并退出？',
                                   parent=self.root):
                self.closing = True
                self.stop()
        else:
            self.root.destroy()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--worker')
    parser.add_argument('--firmware', help='full merged BIN used by --diagnose')
    parser.add_argument('--diagnose', help='Write an offline dependency/firmware report; never opens a COM port.')
    args = parser.parse_args()
    if args.worker:
        return run_worker(args.worker)
    if args.diagnose:
        import esptool
        from esptool.loader import StubFlasher
        firmware_path = args.firmware or default_firmware()
        firmware = Firmware.load(firmware_path) if firmware_path else None
        stub = StubFlasher('esp32s3')
        report = {'tool_version': VERSION, 'esptool_version': esptool.__version__,
                  'firmware_version': firmware.version if firmware else None,
                  'firmware_sha256': firmware.sha256 if firmware else None,
                  'app_sha256': firmware.app_sha256 if firmware else None,
                  'elf_sha256': firmware.elf_sha256 if firmware else None,
                  'embedded_firmware': firmware is not None and Path(firmware_path).parent == resource_root() / 'assets',
                  'ports': [port.device for port in serial.tools.list_ports.comports()],
                  'tcl_version': tk.Tcl().eval('info patchlevel'), 'frozen': bool(getattr(sys, 'frozen', False)),
                  's3_stub_bytes': len(stub.text),
                  'serial_opened': False, 'flash_requested': False}
        Path(args.diagnose).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        return 0
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass
    root = tk.Tk()
    ESPFlasherApp(root)
    root.mainloop()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
