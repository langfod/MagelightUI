@echo off
rem Builds and runs hosttheme_test.exe: the colour rules of src\HostThemeCore.h (0.31.9 keyboard themes) on
rem the desktop, no game. %1 = output folder (default C:\b\mgl-hostthemetest). Exit 0 when every check passes.
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=C:\b\mgl-hostthemetest"
call "%~dp0..\desktop-harness\vcvars.bat" || exit /b 1
set "ROOT=%~dp0..\.."
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /EHsc /std:c++20 /W4 /WX /MD /O2 /I "%ROOT%\src" "%~dp0hosttheme_test.cpp" /Fo"%OUT%\\" /Fe"%OUT%\hosttheme_test.exe" || exit /b 1
"%OUT%\hosttheme_test.exe"
exit /b %ERRORLEVEL%
