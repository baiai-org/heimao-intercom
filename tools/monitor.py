# -*- coding: utf-8 -*-
"""Serial monitor for OpenRadioLink-v0.3.

Reads the board console over the ESP32-S3 native USB-Serial/JTAG port.
Opening the port does NOT reset the chip, so the application keeps running
(the same property that makes flashing work with no button press).

    py -3.9 tools\\monitor.py --port COM5
    py -3.9 tools\\monitor.py --port COM5 --log run.log
    py -3.9 tools\\monitor.py --port COM5 --reset      # classic DTR/RTS reset

Ctrl+C stops it.
"""
import argparse
import datetime
import io
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.stderr.write(
        "pyserial is missing. Run tools\\setup_toolchain.ps1 first.\n")
    sys.exit(2)


def pick_port():
    from serial.tools import list_ports
    cands = list(list_ports.comports())
    for p in cands:
        if (p.vid, p.pid) == (0x303A, 0x1001):
            return p.device
    for p in cands:
        if p.vid == 0x303A:
            return p.device
    if cands:
        return cands[0].device
    sys.stderr.write("no COM port found\n")
    sys.exit(2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None, help="COM5 ... (auto when omitted)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--log", default=None, help="also append raw text to this file")
    ap.add_argument("--reset", action="store_true",
                    help="pulse DTR/RTS once before listening (restarts the app)")
    ap.add_argument("--timestamp", action="store_true")
    ap.add_argument("--audio-out", default=None,
                    help="decode an ---ORL-AUDIO--- hex dump into this raw "
                         "int16 little-endian file")
    ap.add_argument("--seconds", type=float, default=None,
                    help="exit automatically after N seconds (for scripted capture)")
    ap.add_argument("--send", default=None,
                    help="text to write to the board once the port is open, "
                         "e.g. --send R to re-run the bring-up report")
    ap.add_argument("--send-delay", type=float, default=1.5,
                    help="seconds to wait before --send (default 1.5)")
    ap.add_argument("--send2", default=None,
                    help="second text to write later, e.g. --send2 z to STOP a "
                         "transmit test.  Used so the stop command is guaranteed "
                         "by the script rather than by a follow-up invocation.")
    ap.add_argument("--send2-delay", type=float, default=None,
                    help="seconds to wait before --send2 (default: 5 s before "
                         "the time limit, or 10 s if no limit is set)")
    args = ap.parse_args()

    port = args.port or pick_port()
    log = io.open(args.log, "a", encoding="utf-8") if args.log else None

    sys.stdout.write("monitor: %s @ %d 8N1   (Ctrl+C to quit)\n"
                     % (port, args.baud))
    sys.stdout.flush()

    """
    Open the port WITHOUT touching the control lines.

    pyserial asserts DTR and RTS when it opens a port, and the ESP32-S3's
    built-in USB-Serial/JTAG interprets that as a reset request -- so a plain
    `serial.Serial(port)` reboots the target on every connection.  That is
    invisible in a log (you just see a fresh boot banner) but it silently
    destroys any state the firmware was holding: configuration, voice mode,
    counters, an in-progress transmission.

    Setting dtr/rts *before* open() stores the deasserted state, and pyserial
    then opens with both lines low, i.e. a pure listener.
    """
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = args.baud
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()

    start = time.time()
    deadline = (start + args.seconds) if args.seconds else None
    sent = False
    sent2 = False
    if args.send2:
        # Default the second send to shortly before the session ends, which is
        # what makes it a reliable "stop the transmitter" command.
        if args.send2_delay is not None:
            send2_at = start + args.send2_delay
        elif args.seconds:
            send2_at = start + max(args.send_delay + 1.0, args.seconds - 3.0)
        else:
            send2_at = start + 10.0
    else:
        send2_at = None
    audio_samples = []
    in_audio = False
    try:
        if args.reset:
            # Plain chip reset (app boot): toggle RTS only.
            #
            # Asserting BOTH DTR and RTS is what the ESP32-S3's
            # USB-Serial/JTAG ROM reads as "enter download mode", so the obvious
            # dtr+rts sequence drops the board into the bootloader
            # ("waiting for download") and the application never starts.
            # For a reliable restart prefer the in-firmware console 'Q' command.
            ser.rts = True
            time.sleep(0.12)
            ser.rts = False
            time.sleep(0.08)

        buf = b""
        while True:
            if deadline and time.time() >= deadline:
                sys.stdout.write("\nmonitor: time limit reached\n")
                break
            if args.send and not sent and time.time() >= start + args.send_delay:
                ser.write(args.send.encode("utf-8"))
                ser.flush()
                sys.stdout.write("\nmonitor: sent %r\n" % args.send)
                sys.stdout.flush()
                sent = True
            if send2_at and not sent2 and time.time() >= send2_at:
                ser.write(args.send2.encode("utf-8"))
                ser.flush()
                sys.stdout.write("\nmonitor: sent %r (stop)\n" % args.send2)
                sys.stdout.flush()
                sent2 = True

            chunk = ser.read(256)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")

                if args.audio_out:
                    if text.startswith("---ORL-AUDIO"):
                        audio_samples = []
                        in_audio = True
                        continue
                    if text.startswith("---ORL-END---"):
                        in_audio = False
                        with open(args.audio_out, "wb") as fh:
                            for v in audio_samples:
                                fh.write(struct.pack("<H", v))
                        peak = 0
                        for v in audio_samples:
                            s = v - 65536 if v >= 32768 else v
                            peak = max(peak, abs(s))
                        sys.stdout.write(
                            "\nmonitor: audio saved: %d samples, peak %d -> %s\n"
                            % (len(audio_samples), peak, args.audio_out))
                        continue
                    if in_audio:
                        # Strict: only accept lines that are pure hex, so an
                        # interleaved log line can never corrupt the capture.
                        if text and len(text) % 4 == 0 and all(
                                ch in "0123456789ABCDEF" for ch in text):
                            for i in range(0, len(text), 4):
                                audio_samples.append(int(text[i:i + 4], 16))
                        continue

                if args.timestamp:
                    text = "%s %s" % (
                        datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3],
                        text)
                sys.stdout.write(text + "\n")
                if log:
                    log.write(text + "\n")
                    log.flush()
            if buf:
                # flush partial lines so prompts show up immediately
                sys.stdout.write(buf.decode("utf-8", "replace"))
                if log:
                    log.write(buf.decode("utf-8", "replace"))
                    log.flush()
                buf = b""
            sys.stdout.flush()
    except KeyboardInterrupt:
        sys.stdout.write("\nmonitor: stopped\n")
    finally:
        ser.close()
        if log:
            log.close()


if __name__ == "__main__":
    main()
