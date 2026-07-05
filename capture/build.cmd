@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1
if %ERRORLEVEL% NEQ 0 (echo vcvars failed & exit /b 1)
if not exist "build" mkdir "build"
cl.exe /EHsc /W4 /std:c++17 /I include /Fo"build\\" /Fe:capture_test.exe src\test_capture.cpp src\capture_dxgi.cpp src\preprocess.cpp d3d11.lib dxgi.lib user32.lib gdi32.lib
if %ERRORLEVEL% EQU 0 (echo Build OK: capture\capture_test.exe)

cl.exe /EHsc /std:c++17 /Fo"build\\" /Fe:build\window_list.exe src\window_list.cpp user32.lib dwmapi.lib
if %ERRORLEVEL% EQU 0 (echo Build OK: capture\build\window_list.exe)

cl.exe /EHsc /std:c++17 /Fo"build\\" /Fe:build\process_list.exe src\process_list.cpp user32.lib
if %ERRORLEVEL% EQU 0 (echo Build OK: capture\build\process_list.exe)

cl.exe /EHsc /std:c++17 /Fo"build\\" /Fe:build\capture_single.exe src\capture_single.cpp d3d11.lib dxgi.lib windowscodecs.lib user32.lib gdi32.lib ole32.lib
if %ERRORLEVEL% EQU 0 (echo Build OK: capture\build\capture_single.exe)

cl.exe /EHsc /std:c++17 /I include /Fo"build\\" /Fe:build\capture_stream.exe src\capture_stream.cpp d3d11.lib dxgi.lib dwmapi.lib user32.lib gdi32.lib windowsapp.lib
if %ERRORLEVEL% EQU 0 (echo Build OK: capture\build\capture_stream.exe)

cl.exe /EHsc /std:c++17 /I include /Fo"build\\" /Fe:build\capture_h264.exe src\capture_h264.cpp src\mf_encoder.cpp d3d11.lib dxgi.lib dwmapi.lib mfplat.lib mf.lib mfuuid.lib user32.lib gdi32.lib windowsapp.lib ws2_32.lib
if %ERRORLEVEL% EQU 0 (echo Build OK: capture\build\capture_h264.exe)

cl.exe /EHsc /std:c++17 /I include /Fo"build\\" /Fe:build\capture_wgc.exe src\capture_wgc.cpp src\capture_wgc_main.cpp d3d11.lib dxgi.lib user32.lib gdi32.lib windowsapp.lib
if %ERRORLEVEL% EQU 0 (echo Build OK: capture\build\capture_wgc.exe)

REM === Benchmark tools (examples/) ===
pushd ..\examples
if not exist "build" mkdir "build"
cl.exe /EHsc /std:c++17 /I ..\capture\include /I ..\protocol /I ..\common\include /Fo"build\\" /Fe:build\wgc_bench_send.exe wgc_bench_send.cpp ..\capture\src\capture_wgc.cpp d3d11.lib dxgi.lib user32.lib gdi32.lib windowsapp.lib ws2_32.lib
if %ERRORLEVEL% EQU 0 (echo Build OK: examples\build\wgc_bench_send.exe)
rustc wgc_bench_recv.rs -o build\wgc_bench_recv.exe -C opt-level=3
if %ERRORLEVEL% EQU 0 (echo Build OK: examples\build\wgc_bench_recv.exe)
popd
