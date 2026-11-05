@echo off
setlocal
cd /d "%~dp0.."
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "COMPAREEL_VS=%%i"
call "%COMPAREEL_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cmake -S .deps/libheif-src/libheif-1.23.7 -B .deps/libheif-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="%CD%/.deps/libheif-1.23.7" -DFFMPEG_ROOT="%CD%/.deps/ffmpeg-8.1" -DWITH_FFMPEG_DECODER=ON -DWITH_FFMPEG_DECODER_PLUGIN=OFF -DENABLE_PLUGIN_LOADING=OFF -DWITH_LIBDE265=OFF -DWITH_X265=OFF -DWITH_X264=OFF -DWITH_OpenH264_DECODER=OFF -DWITH_AOM_DECODER=OFF -DWITH_AOM_ENCODER=OFF -DWITH_DAV1D=OFF -DWITH_KVAZAAR=OFF -DWITH_LIBSHARPYUV=OFF -DWITH_LIBSHARPYUV_INTERNAL=OFF -DWITH_EXAMPLES=OFF -DWITH_GDK_PIXBUF=OFF -DBUILD_TESTING=OFF -DBUILD_DOCUMENTATION=OFF -DENABLE_PARALLEL_TILE_DECODING=OFF
if errorlevel 1 exit /b 1
cmake --build .deps/libheif-build --target heif --parallel 4
if errorlevel 1 exit /b 1
cmake --install .deps/libheif-build
exit /b %errorlevel%
