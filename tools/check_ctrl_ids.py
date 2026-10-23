#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_ctrl_ids.py —— 校验子控件 ID 不为 0

背景（真实事故）：
    apitest.c 里创建 30 个参数复选框和 30 个值输入框时，把 mk() 的 id 实参
    写成了字面量 0。于是每个控件 ID 都是 0，用户勾选复选框时产生的
    WM_COMMAND 里 wParam 低字也是 0，apitest_on_command() 根本认不出
    是哪个参数，值输入框就一直停在 EnableWindow(FALSE) 的禁用状态 ——
    表现出来就是「勾选了，但文本框依然只读、改不动」。

    这类 bug 编译期完全静默：语法对、告警无、逻辑看起来也通。

判据：
    1) 交互类控件（BUTTON / EDIT / COMBOBOX / SCROLLBAR / LISTBOX）的 ID
       不得为字面量 0 —— 它们要么发 WM_COMMAND，要么发 WM_xSCROLL，
       需要靠 ID 区分。
    2) 同一源文件里，字面量 ID 不得重复（重复同样会让消息无法区分）。

    纯显示控件（STATIC）不受限制，它们不发命令消息。

用法：
    python3 tools/check_ctrl_ids.py src/apitest.c [更多源文件...]
退出码：0 通过，1 发现问题。
"""

import re
import sys

# 需要靠 ID 区分消息的控件类
NEED_ID = {
    "BUTTON", "EDIT", "COMBOBOX", "SCROLLBAR", "LISTBOX",
    "COMBOBOXEX32", "RICHEDIT", "RICHEDIT20A", "RICHEDIT20W",
}

# 直接调 CreateWindowExW 时，菜单/ID 参数在第 10 个位置（下标 9）
CWEX_ID_ARG = 9
# mk(parent, cls, text, style, exstyle, id, font) 的 id 在下标 5
MK_ID_ARG = 5


def split_args(s):
    """按顶层逗号切分参数，跳过字符串与嵌套括号。"""
    args, cur, depth, in_str, esc = [], "", 0, False, False
    for ch in s:
        if in_str:
            cur += ch
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                in_str = False
            continue
        if ch == '"':
            in_str = True
            cur += ch
            continue
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            args.append(cur.strip())
            cur = ""
        else:
            cur += ch
    args.append(cur.strip())
    return args


def find_calls(src, name):
    """找出 name( ... ) 的所有调用，返回 [(起始偏移, 参数字符串)]。"""
    out = []
    for m in re.finditer(re.escape(name) + r"\s*\(", src):
        i = m.end() - 1          # 指向 '('
        depth, j, in_str, esc = 0, i, False, False
        while j < len(src):
            ch = src[j]
            if in_str:
                if esc:
                    esc = False
                elif ch == "\\":
                    esc = True
                elif ch == '"':
                    in_str = False
                j += 1
                continue
            if ch == '"':
                in_str = True
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        if depth == 0:
            out.append((m.start(), src[i + 1:j]))
    return out


def literal_class(arg):
    """从 'L"BUTTON"' 这样的实参里取出类名；不是字面量则返回 None。"""
    m = re.fullmatch(r'L?"([^"]*)"', arg.strip())
    return m.group(1).upper() if m else None


def line_of(src, off):
    return src.count("\n", 0, off) + 1


def check(path):
    try:
        src = open(path, encoding="utf-8").read()
    except OSError as e:
        print("  [跳过] %s：%s" % (path, e))
        return 0, 0

    fails = []

    for name, id_idx in (("ctx->mk", MK_ID_ARG),
                         ("CreateWindowExW", CWEX_ID_ARG)):
        for off, argstr in find_calls(src, name):
            args = split_args(argstr)
            if len(args) <= id_idx:
                continue
            cls = literal_class(args[1])
            if cls is None or cls not in NEED_ID:
                continue
            cid = args[id_idx].replace(" ", "")
            if cid == "0":
                fails.append((line_of(src, off), name, cls,
                              "ID 传了字面量 0"))

    # 字面量 ID 重复检查（只看非 0 的纯数字）
    seen = {}
    for name, id_idx in (("ctx->mk", MK_ID_ARG),
                         ("CreateWindowExW", CWEX_ID_ARG)):
        for off, argstr in find_calls(src, name):
            args = split_args(argstr)
            if len(args) <= id_idx:
                continue
            cls = literal_class(args[1])
            if cls is None or cls not in NEED_ID:
                continue
            cid = args[id_idx].strip()
            if not re.fullmatch(r"\d+", cid) or cid == "0":
                continue
            if cid in seen:
                fails.append((line_of(src, off), name, cls,
                              "ID %s 与第 %d 行重复" % (cid, seen[cid])))
            else:
                seen[cid] = line_of(src, off)

    if fails:
        print("  [失败] %s" % path)
        for ln, name, cls, why in fails:
            print("         第 %-5d 行  %-16s %-10s %s" % (ln, name, cls, why))
    else:
        print("  [通过] %s（交互控件 ID 均非 0 且不重复）" % path)
    return len(fails), len(seen)


def main(argv):
    files = argv[1:] or ["src/apitest.c"]
    print("== 子控件 ID 检查 ==")
    print("  判据：BUTTON/EDIT/COMBOBOX/SCROLLBAR 等交互控件的 ID 不得为 0，")
    print("        且同一文件内字面量 ID 不得重复（否则 WM_COMMAND 无法区分来源）。")
    total = 0
    for f in files:
        bad, _ = check(f)
        total += bad
    print("-" * 64)
    if total:
        print("结论：发现 %d 处问题。控件 ID 为 0 会让消息无法定位到具体控件。" % total)
        return 1
    print("结论：通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
