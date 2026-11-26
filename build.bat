@echo off
setlocal
set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call %VCVARS% >nul || exit /b 1
cl /nologo /std:c++17 /W4 /permissive- /EHsc /O2 /D_CRT_SECURE_NO_WARNINGS miner.cpp /link /SUBSYSTEM:WINDOWS /MACHINE:X64 ws2_32.lib winhttp.lib user32.lib gdi32.lib advapi32.lib shell32.lib /OUT:miner.exe
if errorlevel 1 exit /b 1
echo Compilado: miner.exe
endlocal
