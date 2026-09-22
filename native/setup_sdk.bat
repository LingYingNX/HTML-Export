@echo off
rem Download the WebView2 SDK needed for building.
rem
rem The SDK is not bundled in this repo: WebView2LoaderStatic.lib is 10MB and is
rem Microsoft's copyrighted file, so it should come from the official channel.
rem This script pulls the official NuGet package and copies the three files
rem the build needs into sdk\.
rem
rem Requires PowerShell and curl (both ship with Windows 10 1803+).

setlocal
cd /d "%~dp0"

set "PKGVER=1.0.2903.40"
set "TMPZIP=%TEMP%\webview2-sdk.zip"
set "TMPDIR=%TEMP%\webview2-sdk-extract"

if exist "sdk\WebView2LoaderStatic.lib" (
  echo SDK already present in sdk\ - nothing to do.
  exit /b 0
)

echo Downloading Microsoft.Web.WebView2 %PKGVER% from NuGet...
curl -sL -o "%TMPZIP%" "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/%PKGVER%"
if errorlevel 1 (
  echo Download failed. Check your network connection.
  exit /b 1
)

if not exist "%TMPZIP%" (
  echo Download produced no file.
  exit /b 1
)

echo Extracting...
if exist "%TMPDIR%" rmdir /s /q "%TMPDIR%"
powershell -NoProfile -Command "Expand-Archive -LiteralPath '%TMPZIP%' -DestinationPath '%TMPDIR%' -Force"
if errorlevel 1 (
  echo Extraction failed.
  exit /b 1
)

if not exist "sdk" mkdir "sdk"

copy /y "%TMPDIR%\build\native\include\WebView2.h" "sdk\" >nul
copy /y "%TMPDIR%\build\native\include\WebView2EnvironmentOptions.h" "sdk\" >nul
copy /y "%TMPDIR%\build\native\x64\WebView2LoaderStatic.lib" "sdk\" >nul

del /q "%TMPZIP%" 2>nul
rmdir /s /q "%TMPDIR%" 2>nul

if not exist "sdk\WebView2LoaderStatic.lib" (
  echo SDK files were not copied correctly.
  exit /b 1
)

echo.
echo Done. SDK is in sdk\ - you can now run build_native.bat
endlocal
