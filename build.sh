#!/usr/bin/env bash
#
# build.sh — 一键构建 Windows 可执行文件（可在 macOS / Linux 上交叉编译）
#
# 产物：
#   dist/B64ShaTool_x64.exe   64 位 Windows（推荐）
#   dist/B64ShaTool_x86.exe   32 位 Windows（兼容老机器）
#
# 两个文件都是单文件、静态链接，除 Windows 自带系统 DLL 外无任何依赖，
# 拷贝到完全离线的 Windows 上即可双击运行。
#
# 工具链优先级：
#   1) 项目内置的 llvm-mingw（.toolchain/llvm）—— 自包含，推荐
#   2) PATH 中的 x86_64-w64-mingw32-clang
#   3) PATH 中的 x86_64-w64-mingw32-gcc（GNU 工具链）
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$ROOT/src"
BUILD="$ROOT/build"
DIST="$ROOT/dist"

mkdir -p "$BUILD" "$DIST"

# ------------------------- 1. 定位交叉工具链 -------------------------
TC_KIND=""
TC_BIN=""
PROBE_BIN=""

# 内置工具链候选：本目录优先，其次上层目录（2.0 工作区与根目录共用同一套 llvm-mingw，
# 约 800MB，不重复复制）
TC_LLVM=""
if [ -x "$ROOT/.toolchain/llvm/bin/x86_64-w64-mingw32-clang" ]; then
    TC_LLVM="$ROOT/.toolchain/llvm"
elif [ -x "$ROOT/../.toolchain/llvm/bin/x86_64-w64-mingw32-clang" ]; then
    TC_LLVM="$(cd "$ROOT/../.toolchain/llvm" && pwd)"
fi

if [ -n "$TC_LLVM" ]; then
    TC_KIND="llvm"
    TC_BIN="$TC_LLVM/bin"
    PROBE_BIN="$TC_BIN/x86_64-w64-mingw32-objdump"
elif command -v x86_64-w64-mingw32-clang >/dev/null 2>&1; then
    TC_KIND="llvm"
    TC_BIN="$(dirname "$(command -v x86_64-w64-mingw32-clang)")"
    PROBE_BIN="$TC_BIN/x86_64-w64-mingw32-objdump"
elif command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    TC_KIND="gnu"
    TC_BIN="$(dirname "$(command -v x86_64-w64-mingw32-gcc)")"
    PROBE_BIN="$TC_BIN/x86_64-w64-mingw32-objdump"
elif [ -d "$ROOT/.toolchain/x/mingw-w64" ]; then
    TC_KIND="gnu"
    TC_BIN="$(ls -d "$ROOT/.toolchain/x/mingw-w64"/*/bin | head -1)"
    PROBE_BIN="$TC_BIN/x86_64-w64-mingw32-objdump"
    # 该工具链的 dylib 依赖需要靠环境变量补全（仅用于构建，不影响产物）
    export DYLD_FALLBACK_LIBRARY_PATH="$ROOT/.toolchain/libs${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"
    export LD_LIBRARY_PATH="$ROOT/.toolchain/libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
else
    echo "错误：找不到 mingw-w64 交叉编译器。" >&2
    echo "  macOS:  下载 llvm-mingw 解压到 .toolchain/llvm 下" >&2
    echo "  Debian: sudo apt install gcc-mingw-w64 clang" >&2
    exit 1
fi
echo "[1/7] 工具链：$TC_KIND  ($TC_BIN)"

# ------------------------- 2. 生成图标（如缺失） -------------------------
if [ ! -f "$SRC/app.ico" ]; then
    echo "[2/7] 生成图标 src/app.ico"
    python3 "$ROOT/tools/make_icon.py"
else
    echo "[2/7] 图标已存在，跳过生成"
fi

# ------------------------- 3. 静态校验（本机无法运行 Windows GUI，用静态检查兜底） -------------------------
# 三项检查各自对应一次真实事故：
#   · 父窗口传递 —— build_ui() 曾在 WM_CREATE 期间使用尚未赋值的全局 g_hMain 当父窗口，
#     带 WS_CHILD 的控件全部创建失败，界面一片空白；
#   · 文件 I/O 参数类型 —— read_whole_file() 曾把 size_t 变量取址后强转成 LPDWORD
#     传给 ReadFile，x64 下高 4 字节是未初始化垃圾值，累加后 buf[total]=0 越界写入，
#     点击"开始转换"程序当场退出；
#   · 子控件 ID 为 0 —— 30 个参数复选框的 ID 都传了 0，勾选产生的 WM_COMMAND
#     无法定位到具体参数，值输入框永远停留在禁用状态（表现为"勾选了还是改不动"）。
if command -v python3 >/dev/null 2>&1; then
    echo "[3/7] 静态校验（界面父窗口 / 文件 I/O 参数类型 / 子控件 ID）"
    if ! python3 "$ROOT/tools/check_ui_parent.py" "$SRC/main.c"; then
        echo
        echo "构建中止：界面构建校验未通过，产物极可能仍是空白界面。"
        exit 1
    fi
    if ! python3 "$ROOT/tools/check_io_types.py" "$SRC"/*.c; then
        echo
        echo "构建中止：文件 I/O 参数类型校验未通过，产物可能在编码时崩溃。"
        exit 1
    fi
    if ! python3 "$ROOT/tools/check_ctrl_ids.py" "$SRC"/*.c; then
        echo
        echo "构建中止：子控件 ID 校验未通过，界面上的勾选/按钮会没有反应。"
        exit 1
    fi
else
    echo "[3/7] 未找到 python3，跳过静态校验"
fi

# ------------------------- 4. 编译参数 -------------------------
CFLAGS=(-std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE)
if [ "$TC_KIND" = "gnu" ]; then
    # GNU 工具链需要显式固定源码/执行字符集，保证中文在 Windows 上是正确的 UTF-16
    CFLAGS+=(-finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE)
fi
# clang 的 Windows 目标默认即 UTF-8 窄字符 / UTF-16 宽字符，无需额外选项

LDFLAGS=(-mwindows -municode -static -s)
# 不写入 PE 头里的构建时间戳：否则同样源码每次构建出来的 SHA-256 都不同，
# "传输后核对校验值"这件事就失去了意义。加上它以后构建可复现。
LDFLAGS+=(-Wl,--no-insert-timestamp)
# winhttp：Windows 自带组件，接口测试页用它发请求
LIBS=(-lcomctl32 -lcomdlg32 -lshell32 -lole32 -luser32 -lgdi32 -ladvapi32 -lwinhttp -lkernel32)

build_one() {
    local arch="$1" out="$2"
    local cc="$TC_BIN/${arch}-w64-mingw32-clang"
    local windres="$TC_BIN/${arch}-w64-mingw32-windres"
    local res="$BUILD/app_${arch}.res"

    if [ "$TC_KIND" = "gnu" ]; then
        cc="$TC_BIN/${arch}-w64-mingw32-gcc"
        # GNU windres 会派生 shell 调用预处理器，app.rc 因此写成无注释、
        # 无 #include 的形式，并用 cat 当"预处理"直接透传，绕开该链路。
        echo "      · 编译资源 ($arch)"
        ( cd "$SRC" && "$windres" --preprocessor=cat --codepage=65001 app.rc -O coff -o "$res" )
    else
        echo "      · 编译资源 ($arch)"
        ( cd "$SRC" && "$windres" --codepage=65001 app.rc -O coff -o "$res" )
    fi

    echo "      · 编译并链接 ($arch)"
    "$cc" "${CFLAGS[@]}" \
        "$SRC/main.c" "$SRC/b64sha.c" "$SRC/selftest.c" \
        "$SRC/jsonlite.c" "$SRC/apibody.c" "$SRC/httpclient.c" "$SRC/apitest.c" \
        "$res" \
        -o "$out" "${LDFLAGS[@]}" "${LIBS[@]}"
}

echo "[4/7] 编译 64 位版本"
build_one x86_64 "$DIST/B64ShaTool_x64.exe"

echo "[5/7] 编译 32 位版本"
build_one i686 "$DIST/B64ShaTool_x86.exe"

# ------------------------- 5. 检查依赖 -------------------------
echo "[6/7] 结构体检（PE / 资源 / 导入表）"
if command -v python3 >/dev/null 2>&1; then
    for f in "$DIST/B64ShaTool_x64.exe" "$DIST/B64ShaTool_x86.exe"; do
        python3 "$ROOT/tools/verify_exe.py" "$f" | sed 's/^/      /'
    done
else
    echo "      （未找到 python3，跳过；可直接用 objdump 查看依赖）"
    for f in "$DIST/B64ShaTool_x64.exe" "$DIST/B64ShaTool_x86.exe"; do
        echo "      --- $(basename "$f") ---"
        "$PROBE_BIN" -p "$f" 2>/dev/null | grep -i "DLL Name" | sed 's/^\s*DLL Name: /        /' || true
    done
fi

# ------------------------- 6. 输出校验值 -------------------------
echo "[7/7] 产物信息"
SUMS="$DIST/SHA256SUMS.txt"
{
    echo "# 编码工具箱（Base64 / SHA-256 / 接口测试）构建校验值"
    echo "# 构建时间：$(date '+%Y-%m-%d %H:%M:%S %z')"
    echo "# 工具链  ：$TC_KIND  ($TC_BIN)"
    echo "# 传输到离线机器后，可用  certutil -hashfile <文件名> SHA256  核对以下值"
    echo "# 并在目标机上运行  B64ShaTool_x64.exe /selftest  确认算法正常"
    echo
} > "$SUMS"

for f in "$DIST/B64ShaTool_x64.exe" "$DIST/B64ShaTool_x86.exe"; do
    size=$(wc -c < "$f" | tr -d ' ')
    sum=$( (shasum -a 256 "$f" 2>/dev/null || sha256sum "$f") | cut -d' ' -f1)
    echo "      $(basename "$f")  ${size} 字节"
    echo "        SHA-256: ${sum}"
    printf '%s  %s (%s 字节)\n' "$sum" "$(basename "$f")" "$size" >> "$SUMS"
done

echo
echo "完成。产物目录：$DIST"
echo "  · B64ShaTool_x64.exe   —— 64 位 Windows，单文件免安装"
echo "  · B64ShaTool_x86.exe   —— 32 位 Windows（老机器可用）"
echo "  · SHA256SUMS.txt       —— 传输后核对用校验值"
