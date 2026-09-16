@echo off
setlocal
cd /d "%~dp0"

set "GCC=C:\winprog\C\bin\gcc.exe"
set "WINDRES=C:\winprog\C\bin\windres.exe"
set "OUT=DayZVehicleMap.exe"
set "NEW=build\DayZVehicleMap.new.exe"
set "RESOBJ=build\DayZVehicleMap.res.o"

if not exist "%GCC%" (
    echo ERROR: Compiler not found: %GCC%
    exit /b 1
)
if not exist "%WINDRES%" (
    echo ERROR: Resource compiler not found: %WINDRES%
    exit /b 1
)
if not exist "icon.ico" (
    echo ERROR: Required application icon not found: %CD%\icon.ico
    exit /b 1
)
if not exist "build" mkdir "build"
if errorlevel 1 exit /b 1

del /q "%NEW%" "%RESOBJ%" 2>nul

echo Compiling resources...
"%WINDRES%" --input-format=rc --output-format=coff -I res res\DayZVehicleMap.rc -o "%RESOBJ%"
if errorlevel 1 goto :failed

echo Compiling DayZVehicleMap...
"%GCC%" -std=c17 -O2 -Wall -Wextra -Wpedantic -Wformat=2 -Wstrict-prototypes ^
  -DUNICODE -D_UNICODE -municode -mwindows ^
  src\main.c src\util.c src\app_data.c src\map_view.c src\state.c "%RESOBJ%" ^
  -o "%NEW%" -lgdiplus -lcomctl32 -lgdi32 -luser32 -lkernel32
if errorlevel 1 goto :failed

if not exist "%NEW%" (
    echo ERROR: Compiler reported success but %NEW% was not created.
    goto :failed
)

copy /b /y "%NEW%" "%OUT%" >nul
if errorlevel 1 (
    echo ERROR: Could not replace %OUT%. Is the application still running?
    goto :failed
)

del /q "%NEW%" "%RESOBJ%" 2>nul
echo Build succeeded: %CD%\%OUT%
exit /b 0

:failed
del /q "%NEW%" "%RESOBJ%" 2>nul
echo Build failed. The previous %OUT%, if any, was preserved.
exit /b 1
