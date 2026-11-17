@echo off
rem Builds winmm.dll (Final Foliage Reenabled). Regenerate the proxy/wind sources with tools\gen_winmm_proxy.py and
rem tools\gen_windpatch.py only if Windows' winmm.dll exports or the game build change.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
rc /nologo /fo version.res version.rc || exit /b 1
ml64 /nologo /c /Fo winmm_proxy.obj winmm_proxy.asm || exit /b 1
cl /nologo /LD /O2 /MT /EHa /std:c++17 /W3 /Zi /D_CRT_SECURE_NO_WARNINGS FoliageBend.cpp winmm_proxy.obj version.res /Fe:winmm.dll /link /DLL /DEF:winmm.def /DEBUG /OPT:REF /OPT:ICF /PDBALTPATH:%%_PDB%% kernel32.lib || exit /b 1
