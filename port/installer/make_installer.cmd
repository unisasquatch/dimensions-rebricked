@echo off
rem Builds installer\output\LEGO-Dimensions-Setup.exe: the launcher, staged port files, Inno Setup.
rem Build the port first (build.cmd). setup.exe is always a fresh install: no saves, settings or shader cache from here.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Program Files\LLVM\bin;%PATH%
cd /d "%~dp0"
if not exist build mkdir build
python make_icon.py icon.ico || exit /b 1
rc /nologo /fo build\launcher.res launcher.rc || exit /b 1
clang-cl /nologo /O2 /EHsc /std:c++20 /MT launcher.cpp build\launcher.res /Fo"build\\" /Fe"build\LEGO Dimensions.exe" /link /subsystem:windows user32.lib shell32.lib ole32.lib || exit /b 1
rem ldpkg.exe: unpacks the PS4 .pkg files (setup runs it when "I have PKG files" is picked). Never with the keys built
rem in: setup asks for the keys file, so a shared setup.exe carries none.
clang-cl /nologo /O2 /EHsc /std:c++20 /MT -maes -msse4.1 -DLDPKG_NO_BUILTIN_KEYS ..\tools\ldpkg\ldpkg.cpp /Fo"build\\" /Fe"build\ldpkg.exe" || exit /b 1
for /d %%d in ("%VCToolsRedistDir%x64\Microsoft.VC14*.CRT") do set CRT=%%d
python stage.py "%CRT%" || exit /b 1
"%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" /Q LegoDimensions.iss || exit /b 1
echo built output\LEGO-Dimensions-Setup.exe
