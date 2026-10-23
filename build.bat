@echo off
rem ============================================================================
rem build.bat - 在 Windows 本机上重新编译（可选）
rem
rem 前置条件（任选其一，并把 bin 目录加进 PATH）：
rem   A) LLVM-MinGW 解压包        https://github.com/mstorsjo/llvm-mingw/releases
rem      需要的可执行文件：x86_64-w64-mingw32-clang / x86_64-w64-mingw32-windres
rem   B) MinGW-w64（GCC 版）      例如 https://www.mingw-w64.org 提供的发行包
rem      需要的可执行文件：x86_64-w64-mingw32-gcc / x86_64-w64-mingw32-windres
rem
rem 说明：通常不需要自己编译，dist 目录里的 exe 已是成品。
rem       本脚本只是给需要改代码后重建的同事留一条路。
rem ============================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "SRC=src"
set "BUILD=build"
set "DIST=dist"
if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%DIST%" mkdir "%DIST%"

rem ---- 选择工具链 ----
set "KIND="
where x86_64-w64-mingw32-clang >nul 2>nul && set "KIND=llvm"
if not defined KIND (
    where x86_64-w64-mingw32-gcc >nul 2>nul && set "KIND=gnu"
)
if not defined KIND (
    echo [错误] PATH 里找不到 mingw 交叉编译器，请先安装 LLVM-MinGW 或 MinGW-w64。
    exit /b 1
)
echo [1/4] 工具链：%KIND%

rem ---- 生成图标（如缺失）----
if not exist "%SRC%\app.ico" (
    echo [2/4] 生成图标
    where python >nul 2>nul && python tools\make_icon.py
) else (
    echo [2/4] 图标已存在
)

rem ---- 编译参数 ----
set "CFLAGS=-std=c99 -O2 -Wall -Wextra -DUNICODE -D_UNICODE"
if "%KIND%"=="gnu" set "CFLAGS=%CFLAGS% -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE"
set "LDFLAGS=-mwindows -municode -static -s"
set "LIBS=-lcomctl32 -lcomdlg32 -lshell32 -lole32 -luser32 -lgdi32 -ladvapi32 -lwinhttp -lkernel32"

rem ---- 逐个架构构建 ----
echo [3/4] 编译
call :build x86_64 B64ShaTool_x64.exe
call :build i686   B64ShaTool_x86.exe

echo [4/4] 产物
for %%f in ("%DIST%\B64ShaTool_x64.exe" "%DIST%\B64ShaTool_x86.exe") do (
    echo   %%~nxf
    certutil -hashfile "%%~ff" SHA256 | findstr /v ":"
)
echo 完成。
exit /b 0

:build
set "ARCH=%~1"
set "OUT=%~2"
set "CC=x86_64-w64-mingw32-clang"
set "RC=x86_64-w64-mingw32-windres"
if "%KIND%"=="gnu" (
    set "CC=x86_64-w64-mingw32-gcc"
)
if "%ARCH%"=="i686" (
    if "%KIND%"=="llvm" (set "CC=i686-w64-mingw32-clang") else (set "CC=i686-w64-mingw32-gcc")
    set "RC=i686-w64-mingw32-windres"
)

echo   · 资源 %ARCH%
pushd "%SRC%"
if "%KIND%"=="gnu" (
    "%RC%" --preprocessor=cat --codepage=65001 app.rc -O coff -o "..\%BUILD%\app_%ARCH%.res"
) else (
    "%RC%" --codepage=65001 app.rc -O coff -o "..\%BUILD%\app_%ARCH%.res"
)
popd
if errorlevel 1 exit /b 1

echo   · 链接 %ARCH%
%CC% %CFLAGS% "%SRC%\main.c" "%SRC%\b64sha.c" "%SRC%\selftest.c" ^
     "%SRC%\jsonlite.c" "%SRC%\apibody.c" "%SRC%\httpclient.c" "%SRC%\apitest.c" ^
     "%BUILD%\app_%ARCH%.res" ^
     -o "%DIST%\%OUT%" %LDFLAGS% %LIBS%
if errorlevel 1 exit /b 1
exit /b 0
