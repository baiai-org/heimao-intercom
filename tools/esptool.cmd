@echo off
rem ---------------------------------------------------------------------------
rem  Raw esptool escape hatch for OpenRadioLink-v0.3.
rem  Every argument is passed straight through to esptool 4.8.1 running on the
rem  project-local toolchain (.pylib).
rem
rem    tools\esptool.cmd --chip esp32s3 --port COM5 flash_id
rem    tools\esptool.cmd --chip esp32s3 --port COM5 read_flash 0x0 0x1000 boot.bin
rem
rem  Prefer tools\flash.ps1 for normal work; this is only for odd cases.
rem ---------------------------------------------------------------------------
setlocal
set "PYTHONPATH=%~dp0..\.pylib"
py -3.9 -m esptool %*
exit /b %ERRORLEVEL%
