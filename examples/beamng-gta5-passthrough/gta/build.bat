@echo off
rem Builds GTAxBeam.asi (ScriptHookV script) with MSVC into build\. Run fetch_deps.ps1 first.
rem Uses the newest Visual Studio with the C++ x64 tools; set VCVARS to another vcvars64.bat to pick one.
setlocal
set HERE=%~dp0
if not defined VCVARS for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (echo No Visual Studio with the C++ x64 tools was found. Install "Desktop development with C++" ^(Visual Studio 2022 or its Build Tools^), or set VCVARS to your vcvars64.bat& exit /b 1)
if not exist "%HERE%third_party\shv\ScriptHookV.lib" (echo third_party is missing: run fetch_deps.ps1 first& exit /b 1)
call "%VCVARS%" >nul || exit /b 1
if not exist "%HERE%build" mkdir "%HERE%build"
cl /nologo /LD /O2 /EHsc /std:c++20 /MT /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
  /I "%HERE%third_party\shv" /I "%HERE%third_party\reshade" ^
  "%HERE%src\script.cpp" "%HERE%src\link.cpp" "%HERE%src\compositor.cpp" ^
  /Fo"%HERE%build\\" /Fe"%HERE%build\GTAxBeam.asi" ^
  /link "%HERE%third_party\shv\ScriptHookV.lib" ws2_32.lib user32.lib
