& 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1' -SkipAutomaticLocation | Out-Null
cd 'C:\Users\Andyq\codes\tictactoe\capture'
cl.exe /EHsc /std:c++17 /I include /Fo'build\' /Fe:build\capture_h264.exe src\capture_h264.cpp src\mf_encoder.cpp d3d11.lib dxgi.lib dwmapi.lib mfplat.lib mf.lib mfuuid.lib user32.lib gdi32.lib windowsapp.lib ws2_32.lib
