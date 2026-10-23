#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_io_types.py — 文件 I/O 参数类型静态校验

存在的原因（一次真实事故）：
    read_whole_file() 里曾经写成

        size_t got;
        ...
        if (!ReadFile(h, buf + total, chunk, (LPDWORD)&got, NULL)) { ... }
        total += got;

    ReadFile 的第 4 个参数是 LPDWORD，只写 4 个字节。而 x64 下 size_t 是 8 字节，
    高 4 字节保留栈上的未初始化垃圾值。于是 total += got 得到一个天文数字，
    循环退出后紧接着 buf[total] = 0 就越界写入，触发访问违例 ——
    表现是"选中文件后点开始转换，程序立刻退出"。

    反汇编证据（x86_64，clang -O2）：
        callq *%rdi                  ; ReadFile(...)
        movq  0x38(%rsp), %rax       ; ← 按 8 字节读取该槽位（错误）
        addq  %rax, %rbx             ; total += got
        movb  $0x0, (%r15,%rbx)      ; buf[total] = 0  → 越界

本脚本把规则钉死：
    R1  禁止对 &变量 做指针/定宽整数指针的强制转换；
    R2  ReadFile / WriteFile 的第 4 个实参必须是 &<DWORD 变量>。

不通过时返回非 0，由 build.sh 直接中止构建。
"""

import re
import sys
from pathlib import Path

# 第 4 个参数必须为 DWORD 的 API（下标从 0 起）
API_IO_ARG = {
    "ReadFile": 3,
    "WriteFile": 3,
    # v2.0：WinHTTP 的这几个出参同样是 LPDWORD（只写 4 字节），
    # 若用 size_t 变量取址传入，x64 下高 4 字节会是未初始化垃圾。
    "WinHttpReadData": 3,
    "WinHttpQueryDataAvailable": 1,
    "WinHttpQueryHeaders": 4,
}

# 指针型 typedef（R1）
POINTER_TYPEDEF = r"(?:LP|P)(?:DWORD|LONG|WORD|BYTE|INT|UINT|BOOL|SIZE_T)"

# R1a：指针类型强转取地址，如 (DWORD *)&got / (LONG*)&x
RE_PTR_CAST = re.compile(r"\(\s*([A-Za-z_]\w*)\s*\*\s*\)\s*&\s*([A-Za-z_]\w*)")
# R1b：指针 typedef 强转取地址，如 (LPDWORD)&got
RE_TYPEDEF_CAST = re.compile(r"\(\s*" + POINTER_TYPEDEF + r"\s*\)\s*&")


def strip_comments(src: str) -> str:
    """
    去掉注释，但**逐字保留换行**，这样后续算出来的行号与原文一致。
    字符串/字符字面量内部的内容也一并跳过（避免注释符号出现在字面量里造成误判）。
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "*":
            j = src.find("*/", i + 2)
            end = n if j < 0 else j + 2
            out.append("\n" * src.count("\n", i, end))
            i = end
            continue
        if c == "/" and nxt == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if c in "\"'":
            quote = c
            j = i + 1
            while j < n and src[j] != quote:
                j += 2 if src[j] == "\\" else 1
            seg = src[i:min(j + 1, n)]
            out.append("\n" * seg.count("\n"))   # 合法 C 字面量不含换行，这里只是兜底
            i = min(j + 1, n)
            continue
        out.append(c)
        i += 1
    return "".join(out)


def match_paren(src: str, open_idx: int) -> int:
    depth = 0
    for i in range(open_idx, len(src)):
        if src[i] == "(":
            depth += 1
        elif src[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def split_args(argstr: str):
    args, depth, cur = [], 0, []
    for ch in argstr:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    args.append("".join(cur).strip())
    return args


def collect_dword_vars(src: str):
    """收集所有声明为 DWORD 的变量名。"""
    names = set()
    for m in re.finditer(r"\bDWORD\b([^;{}]*)", src):
        for part in m.group(1).split(","):
            mm = re.match(r"\s*\*?\s*([A-Za-z_]\w*)", part)
            if mm:
                names.add(mm.group(1))
    return names


ISSUES = []


def report(path: Path, src: str, pos: int, msg: str):
    ISSUES.append(f"{path.name}:{src.count(chr(10), 0, pos) + 1}  {msg}")


def check(path: Path):
    src = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    dwords = collect_dword_vars(src)

    # ---------------- R1 ----------------
    for m in RE_PTR_CAST.finditer(src):
        report(path, src, m.start(),
               f"对 &{m.group(2)} 做了指针强转 ({m.group(1)} *)&{m.group(2)}"
               f" —— 读写字长可能不匹配，请改用类型正确的局部变量")
    for m in RE_TYPEDEF_CAST.finditer(src):
        report(path, src, m.start(),
               f"对取地址结果做了强转 {m.group(0).strip()}"
               f" —— 请改用类型正确的局部变量")

    # ---------------- R2 ----------------
    for api, idx in API_IO_ARG.items():
        for m in re.finditer(r"\b" + api + r"\s*\(", src):
            open_idx = src.index("(", m.start())
            close_idx = match_paren(src, open_idx)
            if close_idx < 0:
                continue
            args = split_args(src[open_idx + 1:close_idx])
            if len(args) <= idx:
                report(path, src, m.start(),
                       f"{api}() 实参不足，取不到第 {idx + 1} 个参数")
                continue
            arg = args[idx]
            mm = re.match(r"^&\s*([A-Za-z_]\w*)$", arg)
            if not mm:
                report(path, src, m.start(),
                       f"{api}() 第 {idx + 1} 个实参『{arg}』不是 &<变量> 形式 —— "
                       f"该参数是 LPDWORD（只写 4 字节），必须传入真正的 DWORD 变量地址")
                continue
            if mm.group(1) not in dwords:
                report(path, src, m.start(),
                       f"{api}() 第 {idx + 1} 个实参『&{mm.group(1)}』指向的变量不是 DWORD"
                       f" —— x64 下 8 字节变量会有 4 字节保持未初始化，累加后越界写入")


def main():
    targets = sys.argv[1:]
    if not targets:
        root = Path(__file__).resolve().parent.parent / "src"
        targets = sorted(str(p) for p in root.glob("*.c"))

    checked = 0
    for t in targets:
        p = Path(t)
        if not p.is_file():
            ISSUES.append(f"找不到源文件：{t}")
            continue
        check(p)
        checked += 1

    print("=" * 64)
    print("文件 I/O 参数类型静态校验")
    print("=" * 64)
    print(f"  已检查 {checked} 个源文件")
    if ISSUES:
        for it in ISSUES:
            print("  [失败] " + it)
        print("-" * 64)
        print(f"结论：未通过，共 {len(ISSUES)} 处问题。")
        return 1
    print("  · R1 无对 &变量 的指针强转")
    print("  · R2 ReadFile / WriteFile / WinHTTP 出参均为 DWORD 变量地址")
    print("-" * 64)
    print("结论：通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
