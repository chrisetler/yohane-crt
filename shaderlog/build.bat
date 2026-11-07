@echo off
rem Builds both D3DCOMPILER_43.dll variants next to this script:
rem   D3DCOMPILER_43.dll           diagnostic build (shader dumps, F10/F11 frame captures)
rem   release\D3DCOMPILER_43.dll   release build (CRT filter only)
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
pushd "%~dp0"
cl /nologo /O2 /EHsc /MT /LD shaderlog.cpp /Fe:D3DCOMPILER_43.dll user32.lib /link /DEF:shaderlog.def || (popd & exit /b 1)
if not exist release mkdir release
cl /nologo /O2 /EHsc /MT /LD /DCRT_RELEASE shaderlog.cpp /Fo:release\shaderlog.obj /Fe:release\D3DCOMPILER_43.dll user32.lib /link /DEF:shaderlog.def || (popd & exit /b 1)
del /q shaderlog.obj D3DCOMPILER_43.exp D3DCOMPILER_43.lib release\shaderlog.obj release\D3DCOMPILER_43.exp release\D3DCOMPILER_43.lib 2>nul
popd
