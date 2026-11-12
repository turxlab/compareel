@echo off
setlocal
cd /d "%~dp0.."
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "COMPAREEL_VS=%%i"
call "%COMPAREEL_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not defined COMPAREEL_BUILD_DIR set "COMPAREEL_BUILD_DIR=.build"
if not defined COMPAREEL_DISPLAY_VERSION set "COMPAREEL_DISPLAY_VERSION=1.1.0-dev"
cmake -S . -B "%COMPAREEL_BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%CD%\.deps\Qt\6.8.3\msvc2022_64" -DCOMPAREEL_DISPLAY_VERSION="%COMPAREEL_DISPLAY_VERSION%"
if errorlevel 1 exit /b 1
cmake --build "%COMPAREEL_BUILD_DIR%" %*
exit /b %errorlevel%
