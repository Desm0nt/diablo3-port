@echo off
rem Diablo III Mobile (DM_GAME=d3, org.diablo3.port): build, sign and optionally install the APK (wraps tools\build_apk.ps1; see README.md "Building").
rem   build.bat [-Install <adb serial>] [-Validation]
setlocal
chcp 65001 >nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build_apk.ps1" %*
exit /b %ERRORLEVEL%
