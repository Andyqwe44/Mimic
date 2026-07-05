call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >/dev/null 2>&1
echo VCVARS OK
cl.exe 2>&1
echo CL EXIT: %ERRORLEVEL%
