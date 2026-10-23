#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
静态校验 Win32 界面构建的"父窗口传递"。

为什么需要它：
    本项目在 macOS 上交叉编译，无法在本机运行 Windows GUI 做端到端验证。
    曾经出过一次严重事故 —— build_ui() 把控件父窗口写成全局 g_hMain，而它是在
    WM_CREATE 里调用的（那一刻 g_hMain 尚未赋值、仍为 NULL）。带 WS_CHILD 的窗口
    没有父窗口时系统根本不会创建它，于是所有控件都是 NULL，界面一片空白。

    这个脚本用纯文本静态分析把该性质钉死，改代码后跑一遍即可防止退化。

检查项：
    1. build_ui 的形参必须是 parent，且函数体内不得出现 g_hMain；
    2. 全文件所有带 WS_CHILD 的 CreateWindowExW 都必须传入一个非 NULL 的父窗口；
    3. build_ui 的调用点必须在其之前校验过父窗口句柄有效性；
    4. 主窗口句柄必须由 CreateWindowExW 赋值给 g_hMain。

用法：
    python3 tools/check_ui_parent.py            # 默认检查 src/main.c
"""

import re
import sys
import os

FAIL = []
OK = []


def strip_comments(src):
    """去掉注释与字符串字面量，避免注释里的示例代码干扰判断。"""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                i += 1
        elif c == '/' and i + 1 < n and src[i + 1] == '*':
            i += 2
            while i + 1 < n and not (src[i] == '*' and src[i + 1] == '/'):
                i += 1
            i += 2
        elif c == '"' or c == "'":
            q = c
            out.append(' ')          # 用空格占位，保持字符位置不变
            i += 1
            while i < n:
                if src[i] == '\\':
                    i += 2
                    continue
                if src[i] == q:
                    i += 1
                    break
                i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def find_body(src, signature_re):
    """按大括号配对取出某个函数的函数体文本。"""
    m = re.search(signature_re, src)
    if not m:
        return None, None
    start = src.find('{', m.end() - 1)
    if start < 0:
        return None, None
    depth, i = 0, start
    while i < len(src):
        if src[i] == '{':
            depth += 1
        elif src[i] == '}':
            depth -= 1
            if depth == 0:
                return src[start:i + 1], m.group(0)
        i += 1
    return None, None


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'src/main.c'
    if not os.path.exists(path):
        print('找不到源文件：%s' % path)
        return 2
    raw = open(path, encoding='utf-8').read()
    src = strip_comments(raw)

    # ---- 检查 1：build_ui 形参与函数体内不得使用 g_hMain ----
    body, sig = find_body(src, r'static\s+int\s+build_ui\s*\([^)]*\)')
    if body is None:
        FAIL.append('未找到 build_ui 的定义（应为 static int build_ui(HWND parent)）')
    else:
        if not re.search(r'\(\s*HWND\s+parent\s*\)', sig):
            FAIL.append('build_ui 的形参不是单一的 HWND parent：%s' % sig.strip())
        else:
            OK.append('build_ui 形参为 HWND parent')

        if 'g_hMain' in body:
            FAIL.append('build_ui 函数体内仍然引用了全局 g_hMain —— '
                        'WM_CREATE 期间它是 NULL，会导致所有子控件创建失败')
        else:
            OK.append('build_ui 函数体内不引用 g_hMain')

        # 函数体内每个 mk( 的第一个实参：只允许 parent，或由面板/parent 派生的局部句柄
        local_hwnd = set(re.findall(r'HWND\s+(\w+)\s*=\s*(?:g_panel\s*\[|parent)', body))
        allowed = {'parent'} | local_hwnd
        for call in re.finditer(r'\bmk\s*\(([^,()]*)', body):
            arg = call.group(1).strip()
            if arg not in allowed:
                FAIL.append('build_ui 内 mk() 用了来路不明的父窗口：mk(%s, ...)' % arg)
        if allowed:
            OK.append('build_ui 内 mk() 的父窗口均合法（%s）'
                      % '、'.join(sorted(allowed)))

    # ---- 检查 2：所有 WS_CHILD 的 CreateWindowExW 都要有非 NULL 父窗口 ----
    offender = 0
    for call in re.finditer(r'CreateWindowExW\s*\(', src):
        # 取到匹配的右括号，得到完整实参串
        i = src.index('(', call.start())
        depth, j = 0, i
        while j < len(src):
            if src[j] == '(':
                depth += 1
            elif src[j] == ')':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        args = src[i + 1:j]
        if 'WS_CHILD' not in args:
            continue
        parts = [a.strip() for a in args.split(',')]
        # CreateWindowExW(exstyle, cls, text, style, x, y, w, h, parent, menu, inst, param)
        if len(parts) < 9:
            FAIL.append('带 WS_CHILD 的 CreateWindowExW 实参数不足，无法判定父窗口')
            offender += 1
            continue
        parent = parts[8]
        if parent in ('NULL', '0', ''):
            FAIL.append('带 WS_CHILD 的 CreateWindowExW 传入了 NULL 父窗口 —— '
                        '系统不会创建该窗口：%s' % ' '.join(args.split())[:90])
            offender += 1
        elif parent not in ('parent', 'g_hMain'):
            OK.append('WS_CHILD 窗口使用显式父窗口：%s' % parent)
    if offender == 0:
        OK.append('所有 WS_CHILD 窗口都有有效父窗口')

    # ---- 检查 3：build_ui 的调用点必须已校验父窗口 ----
    sites = []
    for m in re.finditer(r'\bbuild_ui\s*\(', src):
        # 跳过函数定义本身（其前缀形如 "static int "）
        if re.search(r'\bint\s+$', src[:m.start()]):
            continue
        sites.append(m.start())
    if not sites:
        FAIL.append('没有找到 build_ui 的调用点')
    for pos in sites:
        call = src[pos:src.index(')', pos) + 1]
        arg = call[call.index('(') + 1:-1].strip()
        before = src[max(0, pos - 600):pos]
        if arg == 'h':
            # WM_CREATE 内：h 是消息自带的窗口句柄，天然有效
            if 'case WM_CREATE' not in before:
                FAIL.append('build_ui(h) 出现在 WM_CREATE 之外，无法确认 h 有效')
            else:
                OK.append('WM_CREATE 中用消息自带的 h 作为父窗口')
        elif arg == 'g_hMain':
            if re.search(r'if\s*\(\s*!\s*g_hMain\s*\)\s*return', before):
                OK.append('build_ui(g_hMain) 之前已校验 g_hMain 非空')
            else:
                FAIL.append('build_ui(g_hMain) 之前没有校验 g_hMain 是否为空')
        else:
            FAIL.append('build_ui 的调用点父窗口来源不明：build_ui(%s)' % arg)

    # ---- 检查 4：主窗口句柄赋值 ----
    if not re.search(r'g_hMain\s*=\s*CreateWindowExW\s*\(', src):
        FAIL.append('未发现 g_hMain = CreateWindowExW(...)，主窗口句柄来源不明')
    else:
        OK.append('主窗口句柄由 CreateWindowExW 赋值给 g_hMain')

    # ---- 报告 ----
    print('=' * 62)
    print('父窗口传递静态校验 —— %s' % path)
    print('=' * 62)
    for s in OK:
        print('  [通过] %s' % s)
    if FAIL:
        print()
        for s in FAIL:
            print('  [失败] %s' % s)
        print()
        print('结论：未通过（%d 项）—— 界面极可能仍是一片空白。' % len(FAIL))
        return 1
    print()
    print('结论：全部通过。界面构建具备成立条件。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
