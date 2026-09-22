@echo off
setlocal
cd /d "%~dp0"

set "VC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.26100.0"

if not exist "%VC%\bin\Hostx64\x64\cl.exe" (
  echo MSVC compiler not found. Install Visual Studio 2022 Build Tools with "Desktop development with C++".
  exit /b 1
)

set "PATH=%VC%\bin\Hostx64\x64;%SDK%\bin\%SDKVER%\x64;%PATH%"
set "INCLUDE=%VC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\winrt"
set "LIB=%VC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"

if not exist "..\dist" mkdir "..\dist"
if not exist "build_tmp" mkdir "build_tmp"

echo [1/4] generating embedded UI from gui.html...
where node >nul 2>nul
if errorlevel 1 (
  echo Node.js not found - needed to regenerate gui_html.h from gui.html.
  echo Install Node.js, or use the existing gui_html.h.
  exit /b 1
)
node build_ui.js
if errorlevel 1 (
  echo UI generation failed
  exit /b 1
)

echo [2/4] compiling resources (icon + version info)...
if not exist app.ico (
  echo app.ico not found - run make_ico.js first, or the exe will have no icon.
) else (
  rc.exe /nologo /fo "build_tmp\app.res" app.rc
  if errorlevel 1 (
    echo Resource compilation failed
    exit /b 1
  )
)

echo [3/4] compiling...
rem /utf-8 is required: sources contain Chinese comments and UI strings in UTF-8.
rem Without it MSVC assumes the system codepage (GBK here) and mis-parses them.
cl.exe /nologo /std:c++17 /utf-8 /EHsc /O2 /MT /GS- /Gy /DUNICODE /D_UNICODE main.cpp /I sdk /Fe:"..\dist\FrameExporter.exe" /Fo:"build_tmp\\" /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF build_tmp\app.res sdk\WebView2LoaderStatic.lib shlwapi.lib user32.lib gdi32.lib ole32.lib shell32.lib advapi32.lib oleaut32.lib

if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)

del /q build_tmp\*.obj build_tmp\*.res 2>nul
rmdir build_tmp 2>nul
echo [4/4] done
dir /b "..\dist\FrameExporter.exe"
endlocal
