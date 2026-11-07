@echo off
rem Builds both D3DCOMPILER_43.dll variants next to this script:
rem   D3DCOMPILER_43.dll           diagnostic build (shader dumps, F10/F11 frame captures)
rem   release\D3DCOMPILER_43.dll   release build (CRT filter only)
rem Uses the newest Visual Studio / Build Tools install that has the x64 C++ compiler.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo No Visual Studio install with the C++ x64 tools found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
pushd "%~dp0"
cl /nologo /O2 /EHsc /MT /LD shaderlog.cpp /Fe:D3DCOMPILER_43.dll user32.lib /link /DEF:shaderlog.def || (popd & exit /b 1)
if not exist release mkdir release
cl /nologo /O2 /EHsc /MT /LD /DCRT_RELEASE shaderlog.cpp /Fo:release\shaderlog.obj /Fe:release\D3DCOMPILER_43.dll user32.lib /link /DEF:shaderlog.def || (popd & exit /b 1)
del /q shaderlog.obj D3DCOMPILER_43.exp D3DCOMPILER_43.lib release\shaderlog.obj release\D3DCOMPILER_43.exp release\D3DCOMPILER_43.lib 2>nul
popd
