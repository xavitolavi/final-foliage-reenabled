@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /LD /O2 /MT /EHa /std:c++17 /W3 /D_CRT_SECURE_NO_WARNINGS FoliageBend.cpp /Fe:FinalFoliage.dll /link /DLL kernel32.lib
