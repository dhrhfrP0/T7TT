@echo off
rem Builds bin\dinput8.dll (T7Tag) with the installed VS Build Tools.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
if not exist obj mkdir obj
cl /nologo /std:c++17 /utf-8 /O2 /MT /EHa /W3 /DUNICODE /D_UNICODE /LD /Foobj\ ^
   src\dllmain.cpp src\util.cpp src\game.cpp src\focus.cpp src\norage.cpp src\tag.cpp src\overlay.cpp src\select.cpp src\dinput.cpp src\engine.cpp src\model.cpp src\slotlab.cpp src\doll.cpp src\costume.cpp src\recorder.cpp src\pad.cpp ^
   /Fe:bin\dinput8.dll /link /DEF:src\dinput8.def user32.lib kernel32.lib gdi32.lib winmm.lib || exit /b 1
echo BUILD OK
