@echo off
rem Builds AMD FidelityFX FSR 2.2.1's Vulkan backend (tools\fsr2-src, MIT) as static libraries for the port:
rem port\third_party\fsr2\lib\ffx_fsr2_api_x64.lib and ffx_fsr2_api_vk_x64.lib, headers in port\third_party\fsr2\include.
rem Its CMake runs AMD's shader compiler (tools\sc), which breaks on paths with spaces: the build runs from a copy
rem in C:\ffx_fsr2_build. That folder is recorded in the libraries, so it must not be under the user's profile.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not defined VULKAN_SDK set VULKAN_SDK=C:\VulkanSDK\1.4.363.0
set PATH=C:\Program Files\CMake\bin;%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%
set WORK=%SystemDrive%\ffx_fsr2_build
robocopy "%~dp0..\tools\fsr2-src" "%WORK%\src" /E /NFL /NDL /NJH /NJS /NP >nul
set SRC=%WORK%\src\src\ffx-fsr2-api
cmake -S "%SRC%" -B "%WORK%\build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFFX_FSR2_API_DX12=OFF -DFFX_FSR2_API_VK=ON ^
  -DCMAKE_EXE_LINKER_FLAGS=/machine:x64 -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL || exit /b 1
cmake --build "%WORK%\build" || exit /b 1
set DEST=%~dp0third_party\fsr2
if not exist "%DEST%\lib" mkdir "%DEST%\lib"
if not exist "%DEST%\include\vk" mkdir "%DEST%\include\vk"
copy /y "%SRC%\bin\ffx_fsr2_api\*.lib" "%DEST%\lib\" >nul
copy /y "%SRC%\*.h" "%DEST%\include\" >nul
copy /y "%SRC%\vk\ffx_fsr2_vk.h" "%DEST%\include\vk\" >nul
if not exist "%DEST%\include\shaders" mkdir "%DEST%\include\shaders"
copy /y "%SRC%\shaders\ffx_fsr2_resources.h" "%DEST%\include\shaders\" >nul& copy /y "%SRC%\shaders\ffx_fsr2_common.h" "%DEST%\include\shaders\" >nul
copy /y "%WORK%\src\LICENSE.txt" "%DEST%\" >nul
echo FSR 2 built
