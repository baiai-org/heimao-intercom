# -*- coding: utf-8 -*-
"""Generate the board pin map from the *measured* official netlist.

Source of truth: the ESP-IDF-side must match the fabricated PCB, so this script
reads the exported netlist (`.enet`) rather than any hand-written table, and
resolves ESP32-S3-WROOM-1 module pad numbers to GPIO numbers via the vendor
pad table below (cross-checked against the known-good nets).

Outputs:
    docs/PINMAP.md              human readable
    firmware/main/board_pins.h  C header for the firmware
"""
import io
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
# Path to the measured EasyEDA netlist export.  It is not part of the public
# repository; pass it explicitly when regenerating the pin map:
#   ORL_NETLIST=netlist.enet python tools/gen_pinmap.py
ENET = os.environ.get("ORL_NETLIST", "")
DOC = os.path.join(ROOT, "docs", "PINMAP.md")
HDR = os.path.join(ROOT, "firmware", "main", "board_pins.h")

# ESP32-S3-WROOM-1 module pad number -> (GPIO, pad silkscreen name)
PAD = {
    1: (None, "GND"), 2: (None, "3V3"), 3: (None, "EN"),
    4: (4, "IO4"), 5: (5, "IO5"), 6: (6, "IO6"), 7: (7, "IO7"),
    8: (15, "IO15"), 9: (16, "IO16"), 10: (17, "IO17"), 11: (18, "IO18"),
    12: (8, "IO8"), 13: (19, "IO19"), 14: (20, "IO20"), 15: (3, "IO3"),
    16: (46, "IO46"), 17: (9, "IO9"), 18: (10, "IO10"), 19: (11, "IO11"),
    20: (12, "IO12"), 21: (13, "IO13"), 22: (14, "IO14"), 23: (21, "IO21"),
    24: (47, "IO47"), 25: (48, "IO48"), 26: (45, "IO45"), 27: (0, "IO0"),
    28: (35, "IO35"), 29: (36, "IO36"), 30: (37, "IO37"), 31: (38, "IO38"),
    32: (39, "IO39"), 33: (40, "IO40"), 34: (41, "IO41"), 35: (42, "IO42"),
    36: (44, "RXD0/IO44"), 37: (43, "TXD0/IO43"), 38: (2, "IO2"),
    39: (1, "IO1"), 40: (None, "GND"), 41: (None, "GND"),
}

POWER_NETS = {"GND", "+3V3", "AUDIO_3V3", "VBUS_5V", "USB_VBUS_RAW",
              "+5V", "VCC", "3V3"}

# Known-good cross-check: net -> expected GPIO, taken from the verified design
# report (v2.0 pin table + the hand-solder rework report).  If the measured
# netlist disagrees, the pad table above is wrong -> fail loudly.
EXPECT = {
    "USB_DM": 19, "USB_DP": 20, "I2S_MCLK": 4, "I2S_BCLK": 5,
    "I2S_LRCK": 6, "I2S_DIN": 7, "I2S_DOUT": 8,
    "SA818_TXD": 17, "SA818_RXD": 18, "SA818_PD": 14, "SA818_PTT": 21,
    "SA818_AUDIO_IN": 1, "U0TXD": 43, "U0RXD": 44, "BOOT_IO0": 0,
}


def load():
    if not ENET:
        raise SystemExit("Set ORL_NETLIST to the measured EasyEDA netlist export (.enet).")
    with io.open(ENET, encoding="utf-8-sig") as fh:
        enet = json.load(fh)
    comps = {}
    for _uid, c in enet["components"].items():
        pr = c.get("props", {})
        desig = pr.get("Designator")
        pins = {}
        for pin, info in (c.get("pinInfoMap") or {}).items():
            pins[str(pin)] = info.get("net") or ""
        comps[desig] = {
            "lcsc": (pr.get("Supplier Part") or "").strip(),
            "mpn": (pr.get("Manufacturer Part") or "").strip(),
            "pins": pins,
        }
    return comps


def nets_of(comps):
    nets = {}
    for desig, c in comps.items():
        for pin, net in c["pins"].items():
            if net:
                nets.setdefault(net, []).append((desig, pin))
    for net in nets:
        nets[net].sort()
    return nets


def u2_map(comps):
    """net -> gpio for every non-power net on U2."""
    out = {}
    for pin_s, net in comps["U2"]["pins"].items():
        if not net or net in POWER_NETS:
            continue
        pin = int(pin_s)
        gpio, padname = PAD[pin]
        if gpio is None:
            continue
        out[net] = (gpio, pin, padname)
    return out


def cross_check(m):
    bad = []
    for net, gpio in EXPECT.items():
        if net not in m:
            bad.append("%s missing from netlist" % net)
        elif m[net][0] != gpio:
            bad.append("%s: netlist says GPIO%d, expected GPIO%d"
                       % (net, m[net][0], gpio))
    return bad


def main():
    comps = load()
    nets = nets_of(comps)
    m = u2_map(comps)
    bad = cross_check(m)
    if bad:
        sys.stderr.write("PAD TABLE MISMATCH:\n  " + "\n  ".join(bad) + "\n")
        return 2

    # ---------- PINMAP.md ----------
    L = []
    A = L.append
    A("# OpenRadioLink-v0.3 板级引脚映射（由实测网表生成）")
    A("")
    A("> 生成脚本 `tools/gen_pinmap.py`；数据源 `netlist.enet`（官方网表导出）。")
    A("> 引脚号是 **ESP32-S3-WROOM-1 模组焊盘号**，GPIO 号由厂方焊盘表换算。")
    A("> 已与设计报告的既有引脚表交叉校验通过（USB / I2S / SA818 / UART0 / BOOT）。")
    A("")
    A("## 1. U2 主控（ESP32-S3-WROOM-1-**N16R8**）")
    A("")
    A("| 焊盘 | 模组丝印 | GPIO | 网络 | 用途 |")
    A("| --- | --- | --- | --- | --- |")
    use = {
        "USB_DM": "原生 USB D-", "USB_DP": "原生 USB D+",
        "I2S_MCLK": "I2S 主时钟", "I2S_BCLK": "I2S 位时钟",
        "I2S_LRCK": "I2S 帧时钟", "I2S_DIN": "I2S 数据 S3→ES8311",
        "I2S_DOUT": "I2S 数据 ES8311→S3", "SA818_TXD": "SA818 模块 TX",
        "SA818_RXD": "SA818 模块 RX", "SA818_PD": "SA818 使能",
        "SA818_PTT": "SA818 PTT（低=发射）", "SA818_AUDIO_IN": "ADC1_CH0 采 AF_OUT",
        "U0TXD": "UART0 TX（H1 排针）", "U0RXD": "UART0 RX（H1 排针）",
        "BOOT_IO0": "BOOT 键 SW1", "EN": "复位 SW2",
        "LED_STATUS": "状态灯（高=亮）", "AMP_SHDN": "功放关断（LM4871，低=开启）",
        "I2C_SCL": "ES8311 I2C SCL", "I2C_SDA": "ES8311 I2C SDA",
        "CHG_EN": "充电使能（J2.3）", "CHG_STAT": "充电状态（J2.2）",
        "VBAT_ADC": "电池电压采样 ADC1_CH1（J2.1）",
    }
    for pin in sorted(comps["U2"]["pins"], key=int):
        net = comps["U2"]["pins"][pin]
        gpio, padname = PAD[int(pin)]
        gt = "" if gpio is None else "GPIO%d" % gpio
        A("| %s | %s | %s | %s | %s |" % (
            pin, padname, gt, net or "—", use.get(net, "")))
    A("")
    A("## 2. 关键外设引脚")
    A("")
    for desig, title in (("U3", "ES8311 音频编解码器"),
                         ("U4", "LM4871 音频功放"),
                         ("U5", "SA818S-U 射频模块"),
                         ("MK1", "MEMS 麦克风"),
                         ("LED1", "状态灯")):
        if desig not in comps:
            continue
        A("### %s — %s（%s）" % (desig, title, comps[desig]["mpn"]))
        A("")
        A("| 脚 | 网络 |")
        A("| --- | --- |")
        for pin in sorted(comps[desig]["pins"], key=lambda s: (len(s), s)):
            A("| %s | %s |" % (pin, comps[desig]["pins"][pin] or "—"))
        A("")
    A("## 3. 全部网络（56 张）")
    A("")
    A("| 网络 | 脚数 | 连接 |")
    A("| --- | --- | --- |")
    for net in sorted(nets):
        A("| `%s` | %d | %s |" % (
            net, len(nets[net]),
            " ".join("%s.%s" % (d, p) for d, p in nets[net])))
    A("")

    os.makedirs(os.path.dirname(DOC), exist_ok=True)
    with io.open(DOC, "w", encoding="utf-8") as fh:
        fh.write("\n".join(L) + "\n")
    print("wrote", DOC)

    # ---------- board_pins.h ----------
    H = []
    B = H.append
    B("/* Auto-generated by tools/gen_pinmap.py from the measured official")
    B(" * netlist -- DO NOT EDIT BY HAND.  Regenerate after any schematic change.")
    B(" *")
    B(" * Board: OpenRadioLink-v0.3   MCU: ESP32-S3-WROOM-1-N16R8")
    B(" *   Flash 16 MB, PSRAM 8 MB Octal (OPI)   <-- must match sdkconfig")
    B(" */")
    B("#pragma once")
    B("")
    B('#include "driver/gpio.h"')
    B("")
    B("/* ---- ESP32-S3-WROOM-1 module pad -> GPIO (for reference) ---- */")
    for pin in sorted(PAD):
        gpio, padname = PAD[pin]
        B("/* pad %-2d  %-10s %s */" % (
            pin, padname, "GPIO%d" % gpio if gpio is not None else "-"))
    B("")
    B("/* ---- nets attached to the MCU ---- */")
    for net in sorted(m):
        gpio = m[net][0]
        macro = "PIN_" + net
        B("#define %-26s GPIO_NUM_%-3d /* pad %d = %s */" % (
            macro, gpio, m[net][1], m[net][2]))
    B("")
    B("/* ---- !! I2S NAMING TRAP !! ----------------------------------------")
    B(" * On THIS board the I2S nets are named from the CODEC's point of view,")
    B(" * which is the OPPOSITE of ESP-IDF's std-I2S naming:")
    B(" *")
    B(" *   net I2S_DIN  (GPIO7 -> ES8311 pin 9 DSDIN) = S3 -> codec")
    B(" *        -> ESP-IDF field  .dout = PIN_I2S_DIN")
    B(" *   net I2S_DOUT (GPIO8 <- ES8311 pin 7 ASDOUT) = codec -> S3")
    B(" *        -> ESP-IDF field  .din  = PIN_I2S_DOUT")
    B(" *")
    B(" * Swap them and you get silence with no build error.  These aliases")
    B(" * exist so the firmware reads correctly at the call site. */")
    B("#define PIN_I2S_TO_CODEC    PIN_I2S_DIN   /* = .dout */")
    B("#define PIN_I2S_FROM_CODEC  PIN_I2S_DOUT  /* = .din  */")
    B("")
    B("/* ---- polarity / notes from the build reports ---- */")
    B("/* ESP32-S3-WROOM-1-N16R8: 16 MB flash + 8 MB OCT (OPI) PSRAM.   */")
    B("/*   -> GPIO35/36/37 are consumed by the OPI PSRAM and are NC here. */")
    B("/* U4 = LM4871 (net AMP_SHDN, GPIO13): SHUTDOWN IS ACTIVE-HIGH and R14")
    B(" * pulls it up, so the amplifier is OFF from power-up until the firmware")
    B(" * drives PIN_AMP_SHDN LOW.  (Polarity is REVERSED vs the old AW8737.) */")
    B("/* U5 = SA818S (net SA818_PTT, GPIO21): PTT is active-low and R15 pulls it")
    B(" * HIGH, so the radio sits in RX (safe) from power-up; only the firmware")
    B(" * may drive it low, and it must do so only from the PTT state machine. */")
    B("/* LED1 (net LED_STATUS, GPIO12) -> R5 1k -> LED -> GND: active HIGH. */")
    B("/* Native USB (USB-Serial/JTAG) is on GPIO19/20 -> flashing through the")
    B(" * J1 Type-C connector needs no USB-UART bridge and no button press. */")
    B("/* RX audio from the radio (SA818 pin 3 AF_OUT) is sampled by the S3's own")
    B(" * ADC on GPIO1 = ADC1_CH0, NOT through the ES8311.  TX audio (ES8311")
    B(" * DAC_P) is shared: R13 10k -> LM4871, and R16/R17 divider -> SA818 MIC. */")
    B("")

    os.makedirs(os.path.dirname(HDR), exist_ok=True)
    with io.open(HDR, "w", encoding="utf-8") as fh:
        fh.write("\n".join(H) + "\n")
    print("wrote", HDR)

    print("\nU2 net -> GPIO (%d signals):" % len(m))
    for net in sorted(m):
        print("  %-16s GPIO%-3d pad %d" % (net, m[net][0], m[net][1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
