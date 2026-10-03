@echo off
call "%~dp0vcvars.bat" || exit /b 1
cl /nologo /EHsc /std:c++17 /MD /I "%~dp0..\..\extern\ultralight-free-sdk-1.4.1-dev-win-x64\include" probe_appcore.cpp /Fe:probe_appcore.exe /link /LIBPATH:"%~dp0..\..\extern\ultralight-free-sdk-1.4.1-dev-win-x64\lib" AppCore.lib Ultralight.lib WebCore.lib UltralightCore.lib
