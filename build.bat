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

rem Tool locations: RMAC, JAGINC (the folder with Atari's jaguar.inc) and
rem MINGW (its bin) may be set beforehand; these defaults are this machine's.

rem Python is resolved BEFORE MinGW goes on the PATH: mingw64\bin carries its
rem own python.exe, without our packages.
for /f "delims=" %%P in ('where python 2^>nul') do if not defined PY set "PY=%%P"
if not defined PY (echo PYTHON not found on PATH & exit /b 1)

if not defined RMAC set "RMAC=D:\Homebrew\Jaguar\tools\rmac.exe"
if not exist "%RMAC%" (echo RMAC not found at %RMAC% & exit /b 1)
if not defined JAGINC set "JAGINC=D:\Homebrew\Jaguar\include"

rem mingw64\bin FIRST: cc1.exe loads its DLLs from the PATH, and another
rem libwinpthread-1.dll earlier on it makes cc1 die with no message.
if not defined MINGW set "MINGW=C:\msys64\mingw64\bin"
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

rem 2b and 2c compare with skyroads-port itself, frozen at d865dfe1 and
rem unpacked into ref\skyroads-port-main (github.com/ammaarreshi/skyroads-port,
rem MIT; not part of this repository).  Developer checks: without ref\ they
rem are skipped, the cartridge does not depend on them.
if exist ref\skyroads-port-main\src\core\play.c goto :refchecks
echo [2b/2c] skipped: no ref\skyroads-port-main (skyroads-port at d865dfe1)
goto :refdone
:refchecks

rem csrc/port/play.c carries changes for the 68000 (marked JAG:); the
rem reference is skyroads-port's as frozen in ref/.  Every road, random ship
rem states and games, the demo: the whole state equal after every tick.
echo [2b] play.c against the reference, tick by tick
set "REFREN=-Dsr_play_init=ref_sr_play_init -Dsr_play_tick=ref_sr_play_tick -Dsr_play_input=ref_sr_play_input -Dsr_tile_at=ref_sr_tile_at -Dsr_solid=ref_sr_solid -Dsr_in_tunnel=ref_sr_in_tunnel -Dsr_blocktop=ref_sr_blocktop -Dsr_tun_inner=ref_sr_tun_inner -Dsr_tun_outer=ref_sr_tun_outer"
gcc -std=c11 -O2 -c %REFREN% -o build\ref_play.o ref\skyroads-port-main\src\core\play.c
if errorlevel 1 (echo REFERENCE BUILD ERROR & exit /b 1)
gcc -std=c11 -O2 -Wall -Wextra -o build\play_equiv.exe tools\oracle\play_equiv.c csrc\port\play.c build\ref_play.o
if errorlevel 1 (echo EQUIVALENCE BUILD ERROR & exit /b 1)
build\play_equiv.exe cart 400
if errorlevel 1 (echo PLAY.C DIFFERS FROM THE REFERENCE & exit /b 1)

rem The songs: tools/muzax.py's register streams against skyroads-port's
rem driver, tick by tick (corrected for its one-tick-late delays).
echo [2c] MUZAX streams vs skyroads-port's driver
"%PY%" tools\oracle\muzax_check.py %1 2000
if errorlevel 1 (echo MUZAX STREAMS DIFFER & exit /b 1)
:refdone

echo [3/8] memory map, OPL2 tables
"%PY%" tools\memory_map.py
if errorlevel 1 (echo MEMORY MAP ERROR & exit /b 1)
"%PY%" tools\opl2.py
if errorlevel 1 (echo OPL2 TABLES ERROR & exit /b 1)

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

rem Jerry's OPL2: the real DSP binary in the interpreter against its C model
rem (tools/oracle/opl2_model.c), sample for sample: half a second of song 11
rem from its first note, with an effect.  tools\oracle\dsp_check.py 5 11 2
rem is the long run.
echo [6b] DSP OPL2 vs its model
"%PY%" tools\oracle\dsp_check.py 0.5 11 2 now
if errorlevel 1 (echo DSP OPL2 DIFFERS FROM ITS MODEL & exit /b 1)

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
