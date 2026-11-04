@echo off
chcp 936 >nul
setlocal
cd /d "%~dp0"

rem ============================================================
rem  只编译参考线工具（SAI参考线.exe），不影响原有 build.bat
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

echo [1/1] SAI参考线.exe
cl /nologo /utf-8 /EHsc /O2 /MT src\saiguides.cpp /Fe:build\SAI参考线.exe /link user32.lib gdi32.lib comctl32.lib
if errorlevel 1 goto fail

del /q *.obj 2>nul
echo.
echo [OK] 产物在 build\ 目录：
dir /b build\SAI参考线.exe
echo.
pause
exit /b 0

:fail
echo.
echo [x] 构建失败
pause
exit /b 1
