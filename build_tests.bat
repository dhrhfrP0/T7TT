@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
if not exist obj\t mkdir obj\t
cl /nologo /std:c++17 /utf-8 /O2 /MT /EHa /W3 /DUNICODE /D_UNICODE /Foobj\t\ tests\norage_harness.cpp src\norage.cpp /Fe:bin\norage_harness.exe || exit /b 1
cl /nologo /std:c++17 /utf-8 /O2 /MT /EHa /W3 /DUNICODE /D_UNICODE /Foobj\t\ tests\tag_harness.cpp src\tag.cpp src\norage.cpp src\select.cpp /Fe:bin\tag_harness.exe user32.lib || exit /b 1
echo TAG TEST BUILD OK
