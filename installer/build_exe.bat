@echo off
rem Builds BrawlVitaInstaller.exe (one file). Needs Python 3 with Pillow and PyInstaller, and BrawlVita.vpk in the repository folder
rem (build it first with build.bat). The .exe contains only our code and our VPK, never game files.
cd /d "%~dp0"
if not exist "..\BrawlVita.vpk" (
  echo Build BrawlVita.vpk first ^(build.bat in the repository folder^).
  pause
  exit /b 1
)
py -3 -m PyInstaller --noconfirm --onefile --windowed --name BrawlVitaInstaller --add-data "..\BrawlVita.vpk;." brawlvita_installer.py
if errorlevel 1 (
  echo BUILD FAILED
  pause
  exit /b 1
)
copy /y dist\BrawlVitaInstaller.exe ..\BrawlVitaInstaller.exe >nul
echo DONE: BrawlVitaInstaller.exe
