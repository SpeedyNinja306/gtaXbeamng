@echo off
rem Builds GTAxBeamExport.addon64 (ReShade add-on for BeamNG) into build\. Run gta\fetch_deps.ps1 first (ReShade headers).
setlocal
set HERE=%~dp0
set RESHADE_INC=%HERE%..\..\gta\third_party\reshade
if not defined VCVARS for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (echo No Visual Studio with the C++ x64 tools was found; set VCVARS to your vcvars64.bat& exit /b 1)
if not exist "%RESHADE_INC%\reshade.hpp" (echo ReShade headers missing: run gta\fetch_deps.ps1 first& exit /b 1)
call "%VCVARS%" >nul || exit /b 1
if not exist "%HERE%build" mkdir "%HERE%build"
cl /nologo /LD /O2 /EHsc /std:c++20 /MT /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
  /I "%RESHADE_INC%" "%HERE%export.cpp" ^
  /Fo"%HERE%build\\" /Fe"%HERE%build\GTAxBeamExport.addon64" /link user32.lib
