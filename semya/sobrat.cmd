@echo off
rem Сборка компилятора Дана из семени (semya\dan.c) без Python:
rem     semya\sobrat.cmd          - компилятор C из DAN_COMPILER, иначе gcc
rem     semya\sobrat.cmd clang    - указанный компилятор C
rem Готовый компилятор - semya\dan.exe; запускать из корня репозитория.
setlocal
cd /d "%~dp0.."
set "CC=%~1"
if "%CC%"=="" set "CC=%DAN_COMPILER%"
if "%CC%"=="" set "CC=gcc"
"%CC%" -std=c11 -O2 -fwrapv -ffp-contract=off -w semya\dan.c -o semya\dan.exe -lm -Wl,--stack,67108864
if errorlevel 1 exit /b 1
echo Ready: semya\dan.exe
