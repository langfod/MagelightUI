@echo off
call "%~dp0vcvars.bat" || exit /b 1
cl /nologo /EHsc /std:c++20 /MD /I "%~dp0..\..\extern\ul-sdk-2.0.0-beta.2-win-x64-free\include" probe_appcore.cpp /Fe:probe_appcore.exe /link /LIBPATH:"%~dp0..\..\extern\ul-sdk-2.0.0-beta.2-win-x64-free\lib" AppCore.lib Ultralight.lib WebCore.lib UltralightCore.lib
