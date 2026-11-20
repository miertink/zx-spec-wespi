@echo off
rem Assembles the Spectrum code embedded in the firmware (sjasmplus 1.24).
rem The menu goes first: its exported labels (spmenu.exp) are used by the
rem modules. The .bin files are committed; rebuild the firmware after this.
setlocal
cd /d "%~dp0"
if "%SJASMPLUS%"=="" set SJASMPLUS=C:\Espressif\tools\sjasmplus\sjasmplus-1.24.0.win\sjasmplus.exe
"%SJASMPLUS%" --raw=spmenu.bin --exp=spmenu.exp --lst=spmenu.lst spmenu.asm || exit /b 1
"%SJASMPLUS%" --raw=spwifi.bin --lst=spwifi.lst spwifi.asm || exit /b 1
"%SJASMPLUS%" --raw=spinfo.bin --lst=spinfo.lst spinfo.asm || exit /b 1
for %%f in (spmenu.bin spwifi.bin spinfo.bin) do echo %%f %%~zf bytes
