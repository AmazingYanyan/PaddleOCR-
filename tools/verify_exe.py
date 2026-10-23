#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_exe.py — 对构建出的 PE 文件做结构体检

在无法实机运行 Windows 的情况下，用静态方式尽可能确认产物是合规、干净的：
  1. PE 头 / 架构 / 子系统（必须是 GUI 子系统）
  2. 资源表齐全（图标组、版本信息、应用程序清单）
  3. 清单里确实是 asInvoker + 通用控件 v6 + DPI 感知
  4. 导入表只包含 Windows 自带 DLL，不含任何编译器运行库或第三方 DLL
  5. 中文以 UTF-16 正确写入（证明构建时的字符集设置有生效）
  6. 打印 SHA-256 校验值

用法：python3 tools/verify_exe.py dist/B64ShaTool_x64.exe
"""
import hashlib
import os
import struct
import sys

# 允许出现的系统 DLL：都是 Windows 自带组件
ALLOWED_DLLS = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "shell32.dll",
    "comctl32.dll", "comdlg32.dll", "ole32.dll", "oleaut32.dll", "shlwapi.dll",
    "version.dll", "msvcrt.dll", "ntdll.dll", "dwmapi.dll", "uxtheme.dll",
    # v2.0 新增：接口测试页用 WinHTTP 发送请求（Windows 自带组件）
    "winhttp.dll",
}
UCRT_PREFIX = "api-ms-win-crt-"

RT_ICON, RT_GROUP_ICON, RT_VERSION, RT_MANIFEST = 3, 14, 16, 24
RT_NAMES = {RT_ICON: "RT_ICON", RT_GROUP_ICON: "RT_GROUP_ICON",
            RT_VERSION: "RT_VERSION", RT_MANIFEST: "RT_MANIFEST"}

problems = []


def fail(msg):
    problems.append(msg)
    print("  [问题] " + msg)


def load(path):
    with open(path, "rb") as f:
        return f.read()


def pe_info(d):
    if d[:2] != b"MZ":
        raise SystemExit("不是 PE 文件（缺少 MZ 头）")
    pe = struct.unpack("<I", d[0x3C:0x40])[0]
    if d[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("不是 PE 文件（缺少 PE 签名）")
    machine = struct.unpack("<H", d[pe + 4:pe + 6])[0]
    nsec = struct.unpack("<H", d[pe + 6:pe + 8])[0]
    optsz = struct.unpack("<H", d[pe + 20:pe + 22])[0]
    magic = struct.unpack("<H", d[pe + 24:pe + 26])[0]
    subsystem = struct.unpack("<H", d[pe + 24 + 68:pe + 24 + 70])[0]
    entry = struct.unpack("<I", d[pe + 24 + 16:pe + 24 + 20])[0]
    return pe, machine, nsec, optsz, magic, subsystem, entry


def sections(d, pe, nsec, optsz):
    out = []
    so = pe + 24 + optsz
    for i in range(nsec):
        e = so + i * 40
        name = d[e:e + 8].rstrip(b"\0").decode("latin1")
        vsz, va, rsz, ro = struct.unpack("<IIII", d[e + 8:e + 24])
        out.append((name, va, vsz, ro, rsz))
    return out


def rva2off(secs, rva):
    for _, va, vsz, ro, rsz in secs:
        if va <= rva < va + max(vsz, rsz):
            return ro + (rva - va)
    return None


def walk_resources(d, secs, base, off, level, path, found):
    nnamed, nid = struct.unpack("<HH", d[off + 12:off + 16])
    for i in range(nnamed + nid):
        e = off + 16 + i * 8
        nmid, offv = struct.unpack("<II", d[e:e + 8])
        if nmid & 0x80000000:
            no = base + (nmid & 0x7FFFFFFF)
            ln = struct.unpack("<H", d[no:no + 2])[0]
            name = d[no + 2:no + 2 + ln * 2].decode("utf-16le")
        else:
            name = nmid
        if offv & 0x80000000:
            walk_resources(d, secs, base, base + (offv & 0x7FFFFFFF),
                           level + 1, path + [name], found)
        else:
            de = base + offv
            drva, dsz = struct.unpack("<II", d[de:de + 8])
            found.append((path + [name], drva, dsz, rva2off(secs, drva)))


def check_imports(d, secs, dd_off):
    imports = []
    rva, _ = struct.unpack("<II", d[dd_off + 1 * 8:dd_off + 1 * 8 + 8])
    if rva == 0:
        return imports
    o = rva2off(secs, rva)
    while True:
        if d[o:o + 20] == b"\0" * 20:
            break
        name_rva = struct.unpack("<I", d[o + 12:o + 16])[0]
        no = rva2off(secs, name_rva)
        if no is None:
            break
        end = d.index(b"\0", no)
        imports.append(d[no:end].decode("latin1"))
        o += 20
    return imports


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    if not os.path.exists(path):
        print("找不到文件：%s" % path)
        return 2

    d = load(path)
    pe, machine, nsec, optsz, magic, subsystem, entry = pe_info(d)
    secs = sections(d, pe, nsec, optsz)

    print("== 文件：%s ==" % path)
    print("  大小          : %d 字节" % len(d))
    arch = {0x8664: "x86-64 (64 位)", 0x14C: "i386 (32 位)"}.get(machine, hex(machine))
    print("  架构          : %s" % arch)
    print("  PE 类型       : %s" % ("PE32+" if magic == 0x20B else "PE32"))
    print("  子系统        : %d %s" % (subsystem, "(Windows GUI，双击无控制台)" if subsystem == 2 else ""))
    print("  入口 RVA      : 0x%X" % entry)
    if subsystem != 2:
        fail("子系统不是 GUI(2)")

    dd = pe + 24 + (112 if magic == 0x20B else 96)

    # ---- 资源 ----
    rva, _ = struct.unpack("<II", d[dd + 2 * 8:dd + 2 * 8 + 8])
    if rva == 0:
        fail("没有资源表")
        return 1
    base = rva2off(secs, rva)
    found = []
    walk_resources(d, secs, base, base, 0, [], found)

    types = sorted({p[0] for p, _, _, _ in found if p and isinstance(p[0], int)})
    print("  资源类型      : %s" % ", ".join(RT_NAMES.get(t, str(t)) for t in types))
    for need in (RT_GROUP_ICON, RT_VERSION, RT_MANIFEST):
        if need not in types:
            fail("缺少资源 %s" % RT_NAMES[need])
    n_icons = len([1 for p, _, _, _ in found if p and p[0] == RT_ICON])
    print("  图标尺寸数    : %d" % n_icons)
    if n_icons < 3:
        fail("图标尺寸过少（应包含多个尺寸以适配不同显示环境）")

    # ---- 清单 ----
    for p, _, dsz, doff in found:
        if p and p[0] == RT_MANIFEST:
            txt = d[doff:doff + dsz].decode("utf-8", "replace")
            for kw, label in (("asInvoker", "以普通用户权限运行"),
                              ("Common-Controls", "启用系统视觉样式"),
                              ("dpiAware", "声明 DPI 感知")):
                if kw in txt:
                    print("  清单          : 含 %s（%s）" % (kw, label))
                else:
                    fail("清单缺少 %s" % kw)

    # ---- 导入表 ----
    imports = check_imports(d, secs, dd)
    print("  导入 DLL (%d) :" % len(imports))
    bad = []
    for name in sorted(imports, key=str.lower):
        low = name.lower()
        mark = ""
        if low not in ALLOWED_DLLS and not low.startswith(UCRT_PREFIX):
            mark = "   <-- 非系统 DLL"
            bad.append(name)
        print("      %s%s" % (name, mark))
    if bad:
        fail("存在非系统 DLL 依赖：%s（离线机器上可能缺这些库）" % ", ".join(bad))

    # ---- 中文 UTF-16 ----
    if "编码工具箱".encode("utf-16le") in d:
        print("  中文字符串    : 以 UTF-16 正确写入 ✓")
    else:
        fail("未找到预期的中文资源字符串（编码可能有问题）")

    # ---- 校验值 ----
    h = hashlib.sha256(d).hexdigest()
    print("  SHA-256       : %s" % h)

    print()
    if problems:
        print("结论：发现 %d 个问题，请检查。" % len(problems))
        return 1
    print("结论：结构检查全部通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
