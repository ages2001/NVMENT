@echo off
REM nvment9x build (COMMAND.COM: single-line "if" only)
REM Adjust the paths below. Debug build: "set DEBUG=1" first.

set MASM_ROOT=C:\MASM611
set C16_ROOT=C:\MSVC20
set C32_ROOT=C:\MSVC20
set SDKROOT=C:\MSTOOLS
set DDKROOT=C:\DDK
set NVMENT_ROOT=C:\NVMENT

set MASTER_MAKE=1

REM MASTER.MK needs TMP/TEMP
if "%TMP%"=="" set TMP=C:\WINDOWS\TEMP
if "%TEMP%"=="" set TEMP=C:\WINDOWS\TEMP
if not exist "%TMP%" mkdir "%TMP%"
if not exist "%TEMP%" mkdir "%TEMP%"

REM NMAKE must be on PATH before MASTER.MK runs
set PATH=%MASM_ROOT%\BIN;%C32_ROOT%\BIN;%PATH%
REM libc.lib: memcpy/memset/memcmp and the 64-bit arithmetic helpers
set LIB=%C32_ROOT%\LIB;%LIB%

if not exist "%MASM_ROOT%\BIN\ML.EXE" echo [build.bat] WARNING: %MASM_ROOT%\BIN\ML.EXE not found -- MASM_ROOT looks wrong.
if not exist "%C32_ROOT%\BIN\CL.EXE" echo [build.bat] WARNING: %C32_ROOT%\BIN\CL.EXE not found -- C32_ROOT looks wrong.
if not exist "%DDKROOT%\MASTER.MK" echo [build.bat] WARNING: %DDKROOT%\MASTER.MK not found -- DDKROOT looks wrong.
if not exist "%MASM_ROOT%\BIN\NMAKE.EXE" if not exist "%C32_ROOT%\BIN\NMAKE.EXE" echo [build.bat] WARNING: NMAKE.EXE not found under MASM_ROOT or C32_ROOT.

if not exist "%NVMENT_ROOT%\nvmemain.c" echo [build.bat] WARNING: %NVMENT_ROOT%\nvmemain.c not found -- NVMENT_ROOT looks wrong.

cd %NVMENT_ROOT%\9X
nmake