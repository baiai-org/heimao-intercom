# Open-source components

The GUI is adapted from the Tkinter implementation of
[arnaudlvq/ESP32-Flasher](https://github.com/arnaudlvq/ESP32-Flasher),
commit `536704a2c33d4f913da5eb2ddfa56424c9667722` (GPL-3.0).
The original `flasher_tk.py` is preserved in `upstream/`; the modified GUI is
`app.py`. Changes include a Chinese interface, this board's exact ESP32-S3
layout, single merged-BIN validation, selectable keep/factory modes, multiple
selected COM ports, separate concurrent worker processes, progress, cancellation,
and safe skipping of configuration sectors in keep mode.

The actual serial protocol and write/hash verification are provided by
[Espressif esptool](https://github.com/espressif/esptool/tree/v4.8.1), version
4.8.1, GPL-2.0-or-later. esptool is used unchanged. The application is distributed
under GPL-3.0-or-later; see LICENSE. Corresponding GUI source and the esptool
source distribution are included in the release.

Bundled runtime dependencies include CPython/Tkinter/Tcl/Tk, pyserial,
cryptography, cffi, pycparser, ecdsa, six, bitstring, bitarray, tibs, reedsolo,
PyYAML and intelhex. Their licence notices are kept in `licenses/`. Exact
build/runtime versions are listed in `requirements-lock.txt`. PyInstaller 6.16.0
builds the self-contained executable; its bootloader redistribution exception
and the Python/Tcl/Tk licences are included as well. No upstream serial
protocol implementation is reimplemented here.

The release EXE intentionally does **not** embed the firmware image: the
firmware is licensed separately (PolyForm Noncommercial 1.0.0) and bundling it
inside this GPL-3.0 program would place it under the GPL.

Other projects checked:

- [esp-flasher-java](https://github.com/AjinkyaGokhale/esp-flasher-java): the
  upstream Windows release is an installer and still requires external
  Python/esptool; it does not directly meet the offline portable single-EXE goal.
- [stevanStevic/esp32-gui-flasher](https://github.com/stevanStevic/esp32-gui-flasher):
  upstream documentation describes a single selected device and ZIP firmware.
- Espressif Flash Download Tool: useful for factory flashing, but is not the
  open-source GUI base selected for this package.
