@echo off
chcp 936 >nul
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem ============================================================
rem  找不到编译器时可以手动指定，例如：
rem    set VC_VARS=D:\VS\VC\Auxiliary\Build\vcvars64.bat
rem ============================================================
set "VCVARS="
if defined VC_VARS if exist "%VC_VARS%" set "VCVARS=%VC_VARS%"

if not defined VCVARS (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
    )
)

if not defined VCVARS (
    for %%d in (
        "%ProgramFiles%\Microsoft Visual Studio\2022\Community"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional"
    ) do (
        if exist "%%~d\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%~d\VC\Auxiliary\Build\vcvars64.bat"
    )
)

if not defined VCVARS (
    echo [x] 找不到 vcvars64.bat
    echo     请安装 Visual Studio 2019/2022 的「使用 C++ 的桌面开发」工作负载，
    echo     或者先设置环境变量 VC_VARS 指向 vcvars64.bat
    pause
    exit /b 1
)
echo [*] 编译器: %VCVARS%
call "%VCVARS%" >nul
if errorlevel 1 ( echo [x] 初始化编译环境失败 & pause & exit /b 1 )

if not exist build mkdir build

echo [1/3] saimod.dll
cl /nologo /utf-8 /EHsc /O2 /MT src\saimod.cpp /LD /Fe:build\saimod.dll /link /DLL user32.lib gdi32.lib comctl32.lib comdlg32.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib
if errorlevel 1 goto fail

echo [2/3] SAI2启动器.exe
cl /nologo /utf-8 /EHsc /O2 /MT src\launcher.cpp /Fe:build\SAI2启动器.exe user32.lib
if errorlevel 1 goto fail

echo [3/3] sai_inject.exe
cl /nologo /utf-8 /EHsc /O2 /MT src\sai_inject.cpp /Fe:build\sai_inject.exe user32.lib
if errorlevel 1 goto fail

del /q *.obj 2>nul
echo.
echo [OK] 产物在 build\ 目录:
dir /b build
echo.
echo 把 build\ 里的三个文件放到同一目录，再用 sai_path.txt 指定 SAI 主程序路径。
pause
exit /b 0

:fail
echo.
echo [x] 构建失败
pause
exit /b 1
