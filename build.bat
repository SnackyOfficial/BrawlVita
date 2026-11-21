@echo off
cd /d "%~dp0"
echo Building BrawlVita with Docker (the first run downloads the vitasdk image, about 1.6 GB)...
docker info >nul 2>&1
if errorlevel 1 goto no_docker
docker run --rm -v "%cd%:/src" -w /src vitasdk/vitasdk /bin/bash -c "tr -d '\r' < tools/docker_build.sh | bash"
if errorlevel 1 goto fail
echo.
echo DONE. New VPK: %cd%\BrawlVita.vpk
pause
exit /b 0
:no_docker
echo Docker is not running. Start Docker Desktop, wait until it says "running", and run this again.
pause
exit /b 1
:fail
echo.
echo BUILD FAILED. See the messages above.
pause
exit /b 1
