@echo off
setlocal
cd /d "%~dp0"

call build.bat
if errorlevel 1 exit /b 1

echo Running native model and persistence self-test...
start "" /wait "%CD%\DayZVehicleMap.exe" --self-test
if errorlevel 1 (
    echo ERROR: Self-test failed. See self_test_report.txt.
    exit /b 1
)

echo Compiling native GUI smoke harness...
"C:\winprog\C\bin\gcc.exe" -std=c17 -O2 -Wall -Wextra -Wpedantic -Wformat=2 -Wstrict-prototypes ^
  -DUNICODE -D_UNICODE -municode -mwindows tools\ui_smoke.c -o build\ui_smoke.exe ^
  -lgdi32 -luser32 -lkernel32
if errorlevel 1 exit /b 1

echo Running native GUI smoke harness...
start "" /wait "%CD%\build\ui_smoke.exe"
if errorlevel 1 (
    echo ERROR: GUI smoke test failed. See ui_smoke_report.txt.
    exit /b 1
)

echo Compiling frame pacing harness...
"C:\winprog\C\bin\gcc.exe" -std=c17 -O2 -Wall -Wextra -Wpedantic -Wformat=2 -Wstrict-prototypes ^
  -DUNICODE -D_UNICODE tools\frame_pacing_test.c src\util.c src\app_data.c src\map_view.c src\map_objects.c src\state.c ^
  -o build\frame_pacing_test.exe -lgdiplus -lcomctl32 -lgdi32 -luser32 -lkernel32
if errorlevel 1 exit /b 1

echo Running frame pacing harness...
build\frame_pacing_test.exe
if errorlevel 1 exit /b 1

echo Compiling structure rendering harness...
"C:\winprog\C\bin\gcc.exe" -std=c17 -O2 -Wall -Wextra -Wpedantic -Wformat=2 -Wstrict-prototypes ^
  -DUNICODE -D_UNICODE tools\object_render_test.c src\map_objects.c src\util.c ^
  -o build\object_render_test.exe -lgdiplus -lgdi32 -luser32 -lkernel32
if errorlevel 1 exit /b 1

echo Running structure conversion and rendering tests...
"C:\winprog\python\python.exe" tools\test_nasdara_objects.py --renderer build\object_render_test.exe
if errorlevel 1 exit /b 1

echo Compiling real-map cache workload...
"C:\winprog\C\bin\gcc.exe" -std=c17 -O2 -Wall -Wextra -Wpedantic -Wformat=2 -Wstrict-prototypes ^
  -DUNICODE -D_UNICODE tools\object_cache_test.c src\map_view.c src\map_objects.c src\app_data.c src\util.c ^
  -o build\object_cache_test.exe -lgdiplus -lgdi32 -luser32 -lkernel32
if errorlevel 1 exit /b 1

echo Running real-map cache workload...
build\object_cache_test.exe
if errorlevel 1 exit /b 1

echo All tests passed. See self_test_report.txt and ui_smoke_report.txt.
exit /b 0
