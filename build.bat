@echo off
rem ==========================================================================
rem JagSkyroads - full build.  Unattended: exits with an errorlevel.
rem
rem   build.bat [SOURCE]     SOURCE = the SkyRoads freeware zip or folder
rem                          (default: skyroads.zip or gamedata\ in this folder)
rem
rem Order: conversion first (the blobs' real sizes decide the cartridge
rem layout), then the memory map, then the CHECKS, then whatever compiles
rem against them.
rem ==========================================================================
setlocal

for /f "delims=" %%P in ('where python 2^>nul') do if not defined PY set "PY=%%P"
if not defined PY (echo PYTHON not found on PATH & exit /b 1)

set "RMAC=D:\Homebrew\Jaguar\tools\rmac.exe"
if not exist "%RMAC%" (echo RMAC not found at %RMAC% & exit /b 1)
set "JAGINC=D:\Homebrew\Jaguar\include"

if not exist build mkdir build

echo [1/5] data conversion
"%PY%" tools\sr2jag.py %1
if errorlevel 1 (echo CONVERSION ERROR & exit /b 1)

echo [2/5] memory map
"%PY%" tools\memory_map.py
if errorlevel 1 (echo MEMORY MAP ERROR & exit /b 1)

echo [3/5] ambiguous expressions (RMAC has no precedence)
"%PY%" tools\check_expr.py
if errorlevel 1 (echo EXPR ERROR & exit /b 1)

echo [4/5] RMAC
"%RMAC%" -fr -i%JAGINC%\ -iinclude\ -lbuild\main.lst -o build\main.bin src\main.s
if errorlevel 1 (echo ASSEMBLER ERROR & exit /b 1)

echo [5/5] cartridge
"%PY%" tools\cart_build.py
if errorlevel 1 (echo PACK ERROR & exit /b 1)

echo.
echo Build OK.  ROM: build\skyroads.j64
echo Expected on screen: build\preview\m0_expected.png
endlocal
