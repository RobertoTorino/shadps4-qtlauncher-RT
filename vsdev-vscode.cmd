@echo off
echo ================================
echo VS Build Tools environment loaded
echo ================================

call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64

echo.
echo Compiler location:
where cl