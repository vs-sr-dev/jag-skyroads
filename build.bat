@echo off
rem ==========================================================================
rem JagSkyroads - full build.  Unattended: exits with an errorlevel.
rem
rem   build.bat [SOURCE]     SOURCE = the SkyRoads freeware zip or folder
rem                          (default: skyroads.zip or gamedata\ in this folder)
rem
rem Order: conversion first; then the PC oracle, whose expectations are a
rem cartridge blob too; then the memory map, which lays the cartridge out over
rem the blobs' real sizes; then the CHECKS; then whatever compiles against them.
rem ==========================================================================
setlocal

rem Python is resolved BEFORE MinGW goes on the PATH: mingw64\bin carries its
rem own python.exe, without our packages.
for /f "delims=" %%P in ('where python 2^>nul') do if not defined PY set "PY=%%P"
if not defined PY (echo PYTHON not found on PATH & exit /b 1)

set "RMAC=D:\Homebrew\Jaguar\tools\rmac.exe"
if not exist "%RMAC%" (echo RMAC not found at %RMAC% & exit /b 1)
set "JAGINC=D:\Homebrew\Jaguar\include"

rem mingw64\bin FIRST: cc1.exe loads its DLLs from the PATH, and another
rem libwinpthread-1.dll earlier on it makes cc1 die with no message.
set "MINGW=C:\msys64\mingw64\bin"
if not exist "%MINGW%\gcc.exe" (echo MinGW gcc not found at %MINGW% & exit /b 1)
set "PATH=%MINGW%;%PATH%"

if not exist build mkdir build

echo [1/8] data conversion
"%PY%" tools\sr2jag.py %1
if errorlevel 1 (echo CONVERSION ERROR & exit /b 1)

echo [2/8] PC oracle: the attract demo through play.c
gcc -std=c11 -O2 -Wall -Wextra -o build\demo_oracle.exe tools\oracle\demo_oracle.c csrc\port\play.c
if errorlevel 1 (echo ORACLE BUILD ERROR & exit /b 1)
build\demo_oracle.exe cart
if errorlevel 1 (echo ORACLE: THE DEMO DOES NOT COMPLETE & exit /b 1)

echo [3/8] memory map
"%PY%" tools\memory_map.py
if errorlevel 1 (echo MEMORY MAP ERROR & exit /b 1)

echo [4/8] ambiguous expressions (RMAC has no precedence)
"%PY%" tools\check_expr.py
if errorlevel 1 (echo EXPR ERROR & exit /b 1)

echo [5/8] RMAC
"%RMAC%" -fr -i%JAGINC%\ -iinclude\ -lbuild\main.lst -o build\main.bin src\main.s
if errorlevel 1 (echo ASSEMBLER ERROR & exit /b 1)

rem The GPU's composers against road_compose, the C they transcribe: the real
rem binary in a JRISC interpreter, every 37th position of every road (all the
rem phases, a few seconds).  tools\oracle\gpu_check.py 1 = every position.
echo [6/8] GPU composers vs road_compose
"%PY%" tools\oracle\gpu_check.py 37
if errorlevel 1 (echo GPU COMPOSERS DIFFER & exit /b 1)

rem The command goes to the image as ONE string, or it exits 0 doing nothing.
echo [7/8] C blob (m68k-elf-gcc in Docker)
docker run --rm -v "%CD%:/work" -w /work/csrc marsdev-32x:latest "sh build_engine.sh"
if errorlevel 1 (echo C BLOB ERROR & exit /b 1)

echo [8/8] cartridge
"%PY%" tools\cart_build.py
if errorlevel 1 (echo PACK ERROR & exit /b 1)

echo.
echo Build OK.  ROM: build\skyroads.j64
endlocal
