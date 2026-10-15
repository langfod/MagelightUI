@echo off
call "%~dp0vcvars.bat" || exit /b 1
cl /nologo /EHsc /std:c++20 /MD /I "%~dp0..\..\extern\ul-sdk-2.0.0-beta.2-win-x64-free\include" harness.cpp /Fe:harness.exe /link /LIBPATH:"%~dp0..\..\extern\ul-sdk-2.0.0-beta.2-win-x64-free\lib" Ultralight.lib WebCore.lib UltralightCore.lib AppCore.lib
