@echo off
rem Flashes plataboy26 boards with a WCH-LinkE (Windows): the EEPROM library in chunks
rem (writer\writer_NN.bin), then the firmware (flash.bin). Building is a separate step:
rem this script only needs the built files and a flashing tool, no toolchain.
rem
rem   Double-click flash.bat (or: flash.bat [dir]), then connect a board, press a key and
rem   wait for "DONE".
rem
rem   dir   the folder with flash.bin and writer\: by default the folder of this script
rem         (a package made by "make dist"), or firmware\build when run from the repository
rem
rem Needs a flashing tool that you install yourself (none is included or downloaded):
rem   rvprog   pip install rvprog   (Python; the WCH-LinkE needs the WinUSB driver, see README.txt)
rem   wlink    https://github.com/ch32-rs/wlink/releases, in the PATH or next to this script
rem            (uses the WCH-Link driver that WCH-LinkUtility installs)
rem WCH-LinkE in RISC-V mode.
rem
rem Each chunk shows "CHUNK n/N ... OK" on the OLED and beeps shortly; "FAILED" and a long
rem beep mean the EEPROM write failed: program that board again.
setlocal
set "HERE=%~dp0"
set "DIR=%~1"
if not defined DIR if exist "%HERE%flash.bin" set "DIR=%HERE%."
if not defined DIR set "DIR=%HERE%..\build"
if not exist "%DIR%\flash.bin" ( echo %DIR%\flash.bin is missing: build first & pause & exit /b 1 )
if not exist "%DIR%\writer\writer_01.bin" ( echo %DIR%\writer has no writer_NN.bin: build first & pause & exit /b 1 )

rem the tool: rvprog if installed, else wlink (in the PATH, or next to this script)
set "W="
set "ARGS=-f"
for %%I in (rvprog.exe) do if not "%%~$PATH:I"=="" set "W=%%~$PATH:I"
if defined W goto found
set "ARGS=flash -e -a 0x08000000"
for %%I in (wlink.exe) do if not "%%~$PATH:I"=="" set "W=%%~$PATH:I"
if exist "%HERE%wlink.exe" set "W=%HERE%wlink.exe"
if defined W goto found
echo No flashing tool found. Install one of:
echo   rvprog:  pip install rvprog
echo   wlink:   https://github.com/ch32-rs/wlink/releases
echo See README.txt.
pause
exit /b 1

:found
echo flashing from %DIR% with %W%
set N=0

:board
set /a N+=1
echo.
echo ==== Board %N%: connect it and press any key (close this window to stop) ====
pause >nul
for %%F in ("%DIR%\writer\writer_*.bin") do (
  echo   %%~nxF
  set "CUR=%%~nxF"
  "%W%" %ARGS% "%%F"
  if errorlevel 1 goto fail
  rem the writer puts its chunk into the EEPROM
  ping -n 4 127.0.0.1 >nul
)
echo   flash.bin
set "CUR=flash.bin"
"%W%" %ARGS% "%DIR%\flash.bin"
if errorlevel 1 goto fail
echo.
echo   DONE: the game menu should be on the screen (%N% boards so far)
goto board

:fail
echo.
echo   *** FAILED at %CUR% ***  Check the WCH-LinkE cable and the board's power,
echo   then press a key to program this board again from the start.
set /a N-=1
goto board
