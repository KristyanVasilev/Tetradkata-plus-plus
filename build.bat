@echo off
rem Builds build\Tetradkata++.exe with the portable llvm-mingw toolchain in .\toolchain
setlocal
cd /d "%~dp0"
set "TC=%~dp0toolchain\llvm-mingw-20261006-ucrt-x86_64\bin"
if not exist "%TC%\clang++.exe" (
    echo llvm-mingw not found in "%TC%"
    exit /b 1
)
set "PATH=%TC%;%PATH%"
if not exist build mkdir build
if not exist src\tetradkata.ico python tools\make_icon.py || exit /b 1

windres -I src src\tetradkata.rc -O coff -o build\resources.o || exit /b 1
clang++ -std=c++20 -O2 -municode -mwindows -static -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 ^
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers ^
    src\main.cpp src\dialogs.cpp src\editor.cpp src\document.cpp src\fileio.cpp src\lexer.cpp build\resources.o ^
    -o build\Tetradkata++.exe ^
    -lcomctl32 -lcomdlg32 -lshlwapi -lshell32 -limm32 -lbcrypt -ldwmapi -luxtheme -lole32 -luuid -lgdi32 -luser32 || exit /b 1
echo Built build\Tetradkata++.exe
