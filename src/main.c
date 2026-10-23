/*
 * main.c - Base64 / SHA-256 / 接口测试 工具（Windows 原生 Win32 界面）
 *
 * 三个功能区：
 *   ① 文件编码：选择 / 拖入图片、PDF 等任意文件 -> 输出 Base64
 *   ② 解码还原：粘贴 Base64 -> 自动或指定类型 -> 解码保存为文件
 *   ③ 文本摘要：输入任意文本 -> 输出 SHA-256
 * ①②③ 的计算全部在本机完成；④ 接口测试只在你点击「发送请求」时，
 * 把所选文件的内容以 Base64 提交到你填写的那个地址。不写注册表、不留后台进程。
 */
#define UNICODE_GUARD 1   /* UNICODE / _UNICODE 由构建脚本以 -D 传入 */

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wchar.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "b64sha.h"
#include "selftest.h"
#include "apitest.h"

#define APP_TITLE       L"编码工具箱 — Base64 / SHA-256 / 接口测试"
#define PREVIEW_LIMIT   ((size_t)6000000)   /* 超过此长度的 Base64 不再塞进文本框 */
#define B64_LINE_WIDTH  76                  /* MIME 标准换行宽度 */
#define MAX_INPUT_SIZE  ((unsigned long long)1024 * 1024 * 1024)  /* 1GB 上限 */
#define PATHBUF         1024                /* 路径缓冲：留出余量并做长度校验 */

/* ------------------------------ 控件 ID ------------------------------ */
enum {
    ID_TAB = 100,
    /* ① 编码 */
    ID_LBL_SRC, ID_EDT_SRC, ID_BTN_PICK,
    ID_BTN_ENCODE, ID_BTN_COPY_ENC, ID_BTN_SAVE_TXT, ID_BTN_CLEAR_ENC, ID_CHK_WRAP,
    ID_TXT_ENC, ID_LBL_ENC_STATUS,
    /* ② 解码 */
    ID_LBL_DEC_HINT, ID_TXT_DEC,
    ID_BTN_DECODE, ID_BTN_CLEAR_DEC, ID_BTN_OPEN_DIR,
    ID_LBL_TYPE, ID_CMB_TYPE, ID_LBL_OUTDIR, ID_EDT_OUTDIR, ID_BTN_OUTDIR,
    ID_LBL_DEC_STATUS,
    /* ③ 摘要 */
    ID_LBL_HASH_HINT, ID_TXT_HASH_IN,
    ID_BTN_HASH, ID_BTN_CLEAR_HASH, ID_BTN_COPY_HASH, ID_CHK_UPPER,
    ID_LBL_HASH_OUT, ID_TXT_HASH_OUT, ID_LBL_HASH_STATUS,
    ID_STATUS
};

/* ------------------------------ 全局状态 ------------------------------ */
static HINSTANCE g_hInst;
static HWND g_hMain, g_hTab, g_hStatus;
#define PANEL_COUNT 5
static HWND g_panel[PANEL_COUNT];

static HFONT  g_fontUI, g_fontMono;
static HBRUSH g_brPanel;
static int    g_dpi = 96;
static int    g_lineH = 22;

/* ① 编码面板 */
static HWND g_lblSrc, g_edtSrc, g_btnPick, g_btnEncode, g_btnCopyEnc,
            g_btnSaveTxt, g_btnClearEnc, g_chkWrap, g_txtEnc, g_lblEncStatus;
static char  *g_b64;          /* 规范单行 Base64 结果 */
static size_t g_b64Len;
static wchar_t g_srcPath[PATHBUF];
static unsigned long long g_srcSize;
static int    g_encStatusErr;
static int    g_encBusy;      /* 防重入：转换过程中会 pump 消息队列 */

/* ② 解码面板 */
static HWND g_lblDecHint, g_txtDec, g_btnDecode, g_btnClearDec, g_btnOpenDir,
            g_lblType, g_cmbType, g_lblOutDir, g_edtOutDir, g_btnOutDir,
            g_lblDecStatus;
static wchar_t g_outDir[PATHBUF];
static wchar_t g_lastSaved[PATHBUF];    /* 最近保存的文件完整路径 */
static int    g_decStatusErr;
static int    g_decBusy;

/* ③ 摘要面板 */
static HWND g_lblHashHint, g_txtHashIn, g_btnHash, g_btnClearHash,
            g_btnCopyHash, g_chkUpper, g_lblHashOut, g_txtHashOut, g_lblHashStatus;
static int    g_hashStatusErr;
static int    g_hashUpdating;

/* 公共 */
static wchar_t g_lastDir[PATHBUF];     /* 文件对话框初始目录 */

/*
 * 启动自诊断。
 * 界面构件一旦失败，过去的表现是"窗口一片空白、毫无线索"；这里把失败的
 * 次数、错误码和类名都记下来，构建结束即可自证原因，也便于 /diag 远程排查。
 */
static int     g_uiBuilt;             /* 界面是否已构建（防止重复构建） */
static int     g_mkFails;             /* 控件创建失败次数 */
static DWORD   g_mkLastErr;           /* 最近一次控件创建失败的错误码 */
static wchar_t g_mkLastCls[64];       /* 最近一次创建失败的控件类名 */
static HWND    g_buildParent;         /* 构建界面时实际使用的父窗口 */

/* ====================================================================
 * 小工具
 * ==================================================================== */

/*
 * 宽字符格式化统一走 MSVCRT 的 _vsnwprintf：
 *   - %s  对应 wchar_t*（MSVC 语义，与 L"..." 字面量直接配合）
 *   - %I64u 对应 64 位无符号整数（msvcrt.dll 稳定支持，避免 %I64u 的移植差异）
 * 不再使用其它宽字符 printf 变体，规避不同运行库的语义分歧。
 */
static void fmtw(wchar_t *buf, size_t cch, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, cch - 1, fmt, ap);
    va_end(ap);
    buf[cch - 1] = L'\0';
}

static int ds(int v)
{
    return MulDiv(v, g_dpi, 96);
}

static void set_font(HWND h, HFONT f)
{
    if (h && f)
        SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
}

/*
 * 多行文本框必须显式放开长度上限：默认上限偏小，
 * 粘贴大段 Base64 时会被静默截断（截断过的 Base64 解出来就是坏文件）。
 */
static void set_text_limit(HWND h)
{
    if (h)
        SendMessageW(h, EM_SETLIMITTEXT, (WPARAM)0x7FFFFFF0u, 0);
}

static HWND mk(HWND parent, const wchar_t *cls, const wchar_t *text,
               DWORD style, DWORD exstyle, int id, HFONT font)
{
    HWND h;

    /*
     * 父窗口无效时不必调用 API —— 这只会失败，还会覆盖掉真正有用的错误码。
     * 这类"静默失败"正是之前界面空白却查不出原因的根源，所以这里显式记账。
     */
    if (!parent || !IsWindow(parent)) {
        g_mkFails++;
        g_mkLastErr = ERROR_INVALID_WINDOW_HANDLE;
        wcsncpy(g_mkLastCls, cls, 63);
        g_mkLastCls[63] = L'\0';
        return NULL;
    }

    h = CreateWindowExW(exstyle, cls, text, WS_CHILD | WS_VISIBLE | style,
                        0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g_hInst, NULL);
    if (!h) {
        g_mkFails++;
        g_mkLastErr = GetLastError();
        wcsncpy(g_mkLastCls, cls, 63);
        g_mkLastCls[63] = L'\0';
        return NULL;
    }
    set_font(h, font);
    return h;
}

static void set_status(HWND lbl, int *errFlag, int isErr, const wchar_t *text)
{
    if (!lbl)
        return;
    *errFlag = isErr;
    SetWindowTextW(lbl, text);
    InvalidateRect(lbl, NULL, TRUE);
}

static void show_error(const wchar_t *fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    buf[2047] = L'\0';
    MessageBoxW(g_hMain, buf, L"提示", MB_OK | MB_ICONWARNING);
}

static void show_info(const wchar_t *fmt, ...)
{
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 4095, fmt, ap);
    va_end(ap);
    buf[4095] = L'\0';
    MessageBoxW(g_hMain, buf, L"完成", MB_OK | MB_ICONINFORMATION);
}

static void set_wait_cursor(BOOL on)
{
    SetCursor(LoadCursorW(NULL, on ? IDC_WAIT : IDC_ARROW));
}

/* 让界面先刷新再干重活 */
static void pump(void)
{
    MSG m;
    UpdateWindow(g_hMain);
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT)
            break;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

/* ---------- 编码转换 ---------- */

static char *wide_to_utf8(const wchar_t *w, size_t *outLen)
{
    int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *r;
    if (need <= 0) {
        if (outLen) *outLen = 0;
        r = (char *)malloc(1);
        r[0] = '\0';
        return r;
    }
    r = (char *)malloc((size_t)need + 1);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, r, need, NULL, NULL);
    if (outLen) *outLen = (size_t)need - 1;
    return r;
}

static wchar_t *utf8_to_wide(const char *s, size_t len)
{
    int need = MultiByteToWideChar(CP_UTF8, 0, s, (int)len, NULL, 0);
    wchar_t *r;
    if (need <= 0) {
        r = (wchar_t *)malloc(sizeof(wchar_t));
        r[0] = L'\0';
        return r;
    }
    r = (wchar_t *)malloc(((size_t)need + 1) * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, (int)len, r, need);
    r[need] = L'\0';
    return r;
}

/* 取出编辑框全部文本（需 free） */
static wchar_t *get_edit_text(HWND h)
{
    int len = GetWindowTextLengthW(h);
    wchar_t *buf = (wchar_t *)malloc(((size_t)len + 2) * sizeof(wchar_t));
    if (!buf)
        return NULL;
    GetWindowTextW(h, buf, len + 1);
    buf[len] = L'\0';
    return buf;
}

/* ---------- 文件读写 ---------- */

static int read_whole_file(const wchar_t *path, unsigned char **out,
                           unsigned long long *outLen, wchar_t *err, size_t errc)
{
    HANDLE h;
    LARGE_INTEGER size;
    unsigned char *buf;
    size_t total = 0, want;

    *out = NULL;
    *outLen = 0;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fmtw(err, errc, L"无法打开文件（错误码 %lu）。请确认文件存在且未被其它程序占用。",
             GetLastError());
        return 0;
    }
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        fmtw(err, errc, L"无法读取文件大小（错误码 %lu）。", GetLastError());
        return 0;
    }
    if ((unsigned long long)size.QuadPart > MAX_INPUT_SIZE) {
        CloseHandle(h);
        fmtw(err, errc, L"文件过大（%I64u 字节），超过 1GB 处理上限。",
             (unsigned long long)size.QuadPart);
        return 0;
    }

    want = (size_t)size.QuadPart;
    buf = (unsigned char *)malloc(want + 1);
    if (!buf) {
        CloseHandle(h);
        fmtw(err, errc, L"内存不足，无法为 %I64u 字节的文件分配缓冲区。",
             (unsigned long long)want);
        return 0;
    }

    while (total < want) {
        DWORD chunk = (DWORD)((want - total > 0x08000000u) ? 0x08000000u : (want - total));
        /*
         * ReadFile 的第 4 个参数是 LPDWORD —— 它只写 4 个字节，所以必须用一个
         * 真正的 DWORD 变量来接收。
         *
         * 曾经的写法是：size_t got;  ……  ReadFile(..., (LPDWORD)&got, ...)
         * x64 下 size_t 是 8 字节，ReadFile 只覆盖低 4 字节，高 4 字节仍是栈上的
         * 未初始化垃圾值。于是 total += got 得到一个天文数字，循环退出后紧接着
         * 执行 buf[total] = 0 就成了越界写入，触发访问违例 ——
         * 表现就是选中文件后点"开始转换"，程序立刻退出。
         * 反汇编可证实编译器确实按 8 字节读取该槽位：movq 0x38(%rsp), %rax。
         */
        DWORD got = 0;

        if (!ReadFile(h, buf + total, chunk, &got, NULL)) {
            DWORD e = GetLastError();
            free(buf);
            CloseHandle(h);
            fmtw(err, errc, L"读取文件失败（错误码 %lu）。", e);
            return 0;
        }
        if (got == 0)
            break;
        total += got;
    }
    CloseHandle(h);

    /* 防御：循环不变式已保证 total <= want，这里再挡一次，绝不允许越界写入 */
    if (total > want) {
        free(buf);
        fmtw(err, errc, L"内部错误：实际读取长度超出缓冲区大小，已中止。");
        return 0;
    }
    buf[total] = 0;
    *out = buf;
    *outLen = (unsigned long long)total;
    return 1;
}

static int write_whole_file(const wchar_t *path, const void *data, size_t len,
                            wchar_t *err, size_t errc)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    const unsigned char *p = (const unsigned char *)data;
    size_t total = 0;

    if (h == INVALID_HANDLE_VALUE) {
        fmtw(err, errc, L"无法创建文件（错误码 %lu）。请确认目标文件夹存在且有写入权限。",
             GetLastError());
        return 0;
    }
    while (total < len) {
        DWORD chunk = (DWORD)((len - total > 0x08000000u) ? 0x08000000u : (len - total));
        DWORD wrote = 0;
        if (!WriteFile(h, p + total, chunk, &wrote, NULL) || wrote == 0) {
            DWORD e = GetLastError();
            CloseHandle(h);
            DeleteFileW(path);
            fmtw(err, errc, L"写入文件失败（错误码 %lu），已回滚。", e);
            return 0;
        }
        total += wrote;
    }
    CloseHandle(h);
    return 1;
}

/* 目标文件已存在时自动加序号 */
static void make_unique_path(wchar_t *path, size_t cch)
{
    wchar_t dir[PATHBUF], base[PATHBUF], ext[64];
    wchar_t *dot;
    int i;

    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        return;

    wcsncpy(dir, path, PATHBUF - 1);
    dir[PATHBUF - 1] = L'\0';
    {
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (!slash)
            return;
        wcsncpy(base, slash + 1, PATHBUF - 1);
        base[PATHBUF - 1] = L'\0';
        *(slash + 1) = L'\0';
    }
    dot = wcsrchr(base, L'.');
    if (dot) {
        wcsncpy(ext, dot, 63);
        ext[63] = L'\0';
        *dot = L'\0';
    } else {
        ext[0] = L'\0';
    }
    for (i = 1; i < 10000; i++) {
        wchar_t cand[PATHBUF];
        fmtw(cand, PATHBUF, L"%s%s(%d)%s", dir, base, i, ext);
        if (GetFileAttributesW(cand) == INVALID_FILE_ATTRIBUTES) {
            wcsncpy(path, cand, cch - 1);
            path[cch - 1] = L'\0';
            return;
        }
    }
}

/* ---------- 剪贴板 / 对话框 ---------- */

static void copy_text_to_clipboard(const char *utf8, size_t len)
{
    wchar_t *w = utf8_to_wide(utf8, len);
    size_t bytes = (wcslen(w) + 1) * sizeof(wchar_t);
    HGLOBAL mem;

    if (!OpenClipboard(g_hMain)) {
        free(w);
        show_error(L"无法访问剪贴板，请稍后重试。");
        return;
    }
    EmptyClipboard();
    mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void *dst = GlobalLock(mem);
        memcpy(dst, w, bytes);
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    CloseClipboard();
    free(w);
}

static void remember_dir_from_path(const wchar_t *path)
{
    wchar_t tmp[PATHBUF];
    wchar_t *slash;
    wcsncpy(tmp, path, PATHBUF - 1);
    tmp[PATHBUF - 1] = L'\0';
    slash = wcsrchr(tmp, L'\\');
    if (slash) {
        *slash = L'\0';
        wcsncpy(g_lastDir, tmp, PATHBUF - 1);
        g_lastDir[PATHBUF - 1] = L'\0';
    }
}

static int pick_open_file(wchar_t *out, size_t cch)
{
    OPENFILENAMEW ofn;
    wchar_t file[PATHBUF] = L"";
    static const wchar_t filter[] =
        L"图片与 PDF (*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp;*.pdf)\0"
        L"*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp;*.pdf\0"
        L"图片文件 (*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp)\0"
        L"*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp\0"
        L"PDF 文档 (*.pdf)\0*.pdf\0"
        L"所有文件 (*.*)\0*.*\0\0";

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMain;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = PATHBUF;
    ofn.lpstrTitle = L"选择要编码的文件";
    ofn.lpstrInitialDir = g_lastDir[0] ? g_lastDir : NULL;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                OFN_DONTADDTORECENT | OFN_EXPLORER;
    ofn.nFilterIndex = 1;

    if (!GetOpenFileNameW(&ofn))
        return 0;
    wcsncpy(out, file, cch - 1);
    out[cch - 1] = L'\0';
    remember_dir_from_path(out);
    return 1;
}

static int pick_save_txt(wchar_t *out, size_t cch, const wchar_t *defname)
{
    OPENFILENAMEW ofn;
    wchar_t file[PATHBUF];
    static const wchar_t filter[] = L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0\0";

    wcsncpy(file, defname ? defname : L"base64_output.txt", PATHBUF - 1);
    file[PATHBUF - 1] = L'\0';

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMain;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = PATHBUF;
    ofn.lpstrTitle = L"保存为文本文件";
    ofn.lpstrDefExt = L"txt";
    ofn.lpstrInitialDir = g_lastDir[0] ? g_lastDir : NULL;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;

    if (!GetSaveFileNameW(&ofn))
        return 0;
    wcsncpy(out, file, cch - 1);
    out[cch - 1] = L'\0';
    remember_dir_from_path(out);
    return 1;
}

static int pick_folder(HWND owner, const wchar_t *title, wchar_t *out, size_t cch)
{
    BROWSEINFOW bi;
    LPITEMIDLIST pidl;
    wchar_t path[PATHBUF];

    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    pidl = SHBrowseForFolderW(&bi);
    if (!pidl)
        return 0;
    if (!SHGetPathFromIDListW(pidl, path)) {
        CoTaskMemFree(pidl);
        return 0;
    }
    CoTaskMemFree(pidl);
    wcsncpy(out, path, cch - 1);
    out[cch - 1] = L'\0';
    return 1;
}

static void reveal_in_explorer(const wchar_t *path)
{
    wchar_t args[PATHBUF + 32];
    fmtw(args, PATHBUF + 32, L"/select,\"%s\"", path);
    ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
}

static void open_folder(const wchar_t *dir)
{
    ShellExecuteW(NULL, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
}

/* Base64 按固定列宽换行（MIME 风格），返回需 free 的缓冲 */
static char *wrap_text(const char *src, size_t srcLen, size_t width, size_t *outLen)
{
    size_t lines, i, o = 0;
    char *r;

    if (srcLen == 0) {
        r = (char *)malloc(1);
        r[0] = '\0';
        if (outLen) *outLen = 0;
        return r;
    }
    lines = (srcLen + width - 1) / width;
    r = (char *)malloc(srcLen + lines * 2 + 2);
    if (!r) {
        if (outLen) *outLen = 0;
        return NULL;               /* 内存不足：交给调用方判空，绝不越界写 */
    }
    for (i = 0; i < srcLen; i++) {
        if (i > 0 && i % width == 0) {
            r[o++] = '\r';
            r[o++] = '\n';
        }
        r[o++] = src[i];
    }
    r[o] = '\0';
    if (outLen) *outLen = o;
    return r;
}

/* 把内存中的 Base64 结果按当前选项刷进文本框（超长时只放占位提示） */
static void render_b64_to_box(void)
{
    int wrap = (SendMessageW(g_chkWrap, BM_GETCHECK, 0, 0) == BST_CHECKED);
    size_t shown;
    char *text;

    if (!g_b64 || g_b64Len == 0) {
        SetWindowTextW(g_txtEnc, L"");
        return;
    }
    if (g_b64Len > PREVIEW_LIMIT) {
        wchar_t msg[512];
        fmtw(msg, 512,
             L"【内容过长，未在此处显示】\r\n\r\n"
             L"Base64 总长度 %I64u 字符，已超过文本框预览上限 %I64u 字符。\r\n"
             L"完整内容已就绪：请点击「复制结果」或「保存为 TXT」获取。\r\n"
             L"（解码时可自动忽略换行，无需担心换行问题）",
             (unsigned long long)g_b64Len, (unsigned long long)PREVIEW_LIMIT);
        SetWindowTextW(g_txtEnc, msg);
        return;
    }

    if (wrap) {
        text = wrap_text(g_b64, g_b64Len, B64_LINE_WIDTH, &shown);
        if (!text) {
            SetWindowTextW(g_txtEnc,
                           L"【内存不足，无法生成预览】\r\n\r\n"
                           L"Base64 结果已在内存中就绪，请用「复制结果」或「保存为 TXT」获取。");
            return;
        }
    } else {
        text = (char *)malloc(g_b64Len + 1);
        memcpy(text, g_b64, g_b64Len + 1);
        shown = g_b64Len;
    }
    {
        wchar_t *w = utf8_to_wide(text, shown);
        SetWindowTextW(g_txtEnc, w);
        free(w);
    }
    free(text);
}

/* 取出当前应被复制 / 保存的 Base64 文本（需 free） */
static char *current_b64_text(size_t *outLen)
{
    int wrap = (SendMessageW(g_chkWrap, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (!g_b64) {
        char *e = (char *)malloc(1);
        e[0] = '\0';
        if (outLen) *outLen = 0;
        return e;
    }
    if (wrap) {
        char *r = wrap_text(g_b64, g_b64Len, B64_LINE_WIDTH, outLen);
        if (r)
            return r;
        /* 内存不足时退化为不换行的输出，绝不返回空指针给调用方 */
    }
    {
        char *c = (char *)malloc(g_b64Len + 1);
        if (!c) {
            char *e = (char *)malloc(1);
            e[0] = '\0';
            if (outLen) *outLen = 0;
            return e;
        }
        memcpy(c, g_b64, g_b64Len + 1);
        if (outLen) *outLen = g_b64Len;
        return c;
    }
}

static void format_size(unsigned long long n, wchar_t *buf, size_t cch)
{
    if (n < 1024)
        fmtw(buf, cch, L"%I64u 字节", n);
    else if (n < 1024ull * 1024)
        fmtw(buf, cch, L"%.1f KB（%I64u 字节）", (double)n / 1024.0, n);
    else
        fmtw(buf, cch, L"%.2f MB（%I64u 字节）", (double)n / (1024.0 * 1024.0), n);
}

/* ====================================================================
 * ① 文件 -> Base64
 * ==================================================================== */

static void update_src_info(void)
{
    wchar_t buf[1024], sz[128];

    if (!g_srcPath[0]) {
        set_status(g_lblEncStatus, &g_encStatusErr, 0,
                   L"尚未选择文件。可点击「选择文件…」或直接把文件拖到窗口里。");
        return;
    }
    format_size(g_srcSize, sz, 128);
    {
        const wchar_t *slash = wcsrchr(g_srcPath, L'\\');
        fmtw(buf, 1024, L"已选择：%s　·　%s　·　点击「开始转换」生成 Base64",
             slash ? slash + 1 : g_srcPath, sz);
    }
    set_status(g_lblEncStatus, &g_encStatusErr, 0, buf);
}

static void set_source_file(const wchar_t *path)
{
    HANDLE h;
    LARGE_INTEGER sz;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        show_error(L"无法读取该文件：\n%s\n\n错误码 %lu", path, GetLastError());
        return;
    }
    if (!GetFileSizeEx(h, &sz)) {
        CloseHandle(h);
        show_error(L"无法获取文件大小（错误码 %lu）。", GetLastError());
        return;
    }
    CloseHandle(h);

    wcsncpy(g_srcPath, path, PATHBUF - 1);
    g_srcPath[PATHBUF - 1] = L'\0';
    g_srcSize = (unsigned long long)sz.QuadPart;
    remember_dir_from_path(path);
    SetWindowTextW(g_edtSrc, g_srcPath);
    update_src_info();
}

static void do_encode(void)
{
    unsigned char *raw = NULL;
    unsigned long long rawLen = 0;
    wchar_t err[512], stat[1024], szIn[128];
    size_t encLen;

    if (!g_srcPath[0]) {
        show_error(L"请先选择要编码的文件。");
        return;
    }
    if (g_encBusy)
        return;                  /* 上一次转换还没结束，忽略重复点击 */
    g_encBusy = 1;

    set_wait_cursor(TRUE);
    set_status(g_lblEncStatus, &g_encStatusErr, 0, L"正在读取文件…");
    pump();

    if (!read_whole_file(g_srcPath, &raw, &rawLen, err, 512)) {
        set_wait_cursor(FALSE);
        set_status(g_lblEncStatus, &g_encStatusErr, 1, err);
        show_error(L"%s", err);
        g_encBusy = 0;
        return;
    }

    encLen = b64_encoded_len((size_t)rawLen);
    free(g_b64);
    g_b64 = (char *)malloc(encLen + 1);
    if (!g_b64) {
        free(raw);
        set_wait_cursor(FALSE);
        g_encBusy = 0;
        show_error(L"内存不足，无法完成编码。");
        return;
    }
    b64_encode(raw, (size_t)rawLen, g_b64);
    g_b64Len = encLen;
    free(raw);

    set_status(g_lblEncStatus, &g_encStatusErr, 0, L"正在生成文本…");
    pump();
    render_b64_to_box();

    format_size((unsigned long long)rawLen, szIn, 128);
    {
        const wchar_t *slash = wcsrchr(g_srcPath, L'\\');
        fmtw(stat, 1024, L"编码完成：%s　→　Base64 共 %I64u 个字符（原文件 %s）",
             slash ? slash + 1 : g_srcPath, (unsigned long long)encLen, szIn);
    }
    set_status(g_lblEncStatus, &g_encStatusErr, 0, stat);
    set_wait_cursor(FALSE);
    g_encBusy = 0;
}

/* ====================================================================
 * ② Base64 -> 文件
 * ==================================================================== */

static int is_b64_char(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '-' || c == '_';
}

static int is_space_char(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '\f' || c == '\v';
}

/* 包一层防重入：解码过程中会 pump 消息队列，重复点击可能造成二次进入 */
static void do_decode_impl(void);
static void switch_tab(int index);
static void init_default_outdir(void);

static void do_decode(void)
{
    if (g_decBusy)
        return;
    g_decBusy = 1;
    do_decode_impl();
    g_decBusy = 0;
}

static void do_decode_impl(void)
{
    wchar_t *txt = get_edit_text(g_txtDec);
    char *u8 = NULL, *clean = NULL;
    unsigned char *data = NULL;
    size_t u8len = 0, cleanLen = 0, i, ignored = 0, dlen;
    size_t cap;
    int sel, autoType;
    char ext[16];
    const char *detectedExt = NULL, *detectedDesc = NULL;
    wchar_t wdesc[128];
    wchar_t stat[1024], err[512];
    wchar_t fullPath[PATHBUF], fname[128];
    SYSTEMTIME st;
    int ok;

    if (!txt || txt[0] == L'\0') {
        free(txt);
        show_error(L"请先在文本框中粘贴 Base64 内容。");
        return;
    }

    set_wait_cursor(TRUE);
    set_status(g_lblDecStatus, &g_decStatusErr, 0, L"正在解析 Base64…");
    pump();

    u8 = wide_to_utf8(txt, &u8len);
    free(txt);

    clean = (char *)malloc(u8len + 1);
    if (!clean) {
        free(u8);
        set_wait_cursor(FALSE);
        show_error(L"内存不足。");
        return;
    }
    for (i = 0; i < u8len; i++) {
        unsigned char c = (unsigned char)u8[i];
        if (is_b64_char(c) || c == '=' || is_space_char(c))
            clean[cleanLen++] = (char)c;
        else
            ignored++;              /* 全角空格、NBSP、标点等一律忽略并计数 */
    }
    free(u8);

    if (cleanLen == 0) {
        free(clean);
        set_wait_cursor(FALSE);
        set_status(g_lblDecStatus, &g_decStatusErr, 1, L"未找到有效的 Base64 内容。");
        show_error(L"输入内容里没有可识别的 Base64 字符。\n\n"
                   L"请确认粘贴的是文件的 Base64 编码，而不是其它文本。");
        return;
    }

    cap = b64_decoded_cap(cleanLen);
    data = (unsigned char *)malloc(cap + 16);
    if (!data) {
        free(clean);
        set_wait_cursor(FALSE);
        show_error(L"内存不足，无法为解码结果分配缓冲区。");
        return;
    }

    dlen = b64_decode(clean, cleanLen, data, cap + 16);
    free(clean);
    if (dlen == (size_t)-1) {
        free(data);
        set_wait_cursor(FALSE);
        set_status(g_lblDecStatus, &g_decStatusErr, 1, L"Base64 内容无法解析。");
        show_error(L"Base64 内容无法解析，请检查是否被截断或含有非法字符。");
        return;
    }
    if (dlen == 0) {
        free(data);
        set_wait_cursor(FALSE);
        set_status(g_lblDecStatus, &g_decStatusErr, 1, L"解码结果为空（0 字节）。");
        show_error(L"解码结果为 0 字节，未生成文件。");
        return;
    }

    sel = (int)SendMessageW(g_cmbType, CB_GETCURSEL, 0, 0);
    autoType = (sel <= 0);
    if (autoType) {
        detectedExt = detect_file_ext(data, dlen, &detectedDesc);
        snprintf(ext, sizeof(ext), "%s", detectedExt);
    } else {
        const char *e = filetype_option_ext((size_t)sel);
        snprintf(ext, sizeof(ext), ".%s", e ? e : "bin");
        detect_file_ext(data, dlen, &detectedDesc);   /* 仍然识别，用于提示真实类型 */
    }
    if (ext[0] == '.')
        memmove(ext, ext + 1, strlen(ext));

    wcscpy(wdesc, L"未知");
    if (detectedDesc) {
        wchar_t *t = utf8_to_wide(detectedDesc, strlen(detectedDesc));
        wcsncpy(wdesc, t, 127);
        wdesc[127] = L'\0';
        free(t);
    }

    if (!g_outDir[0]) {
        free(data);
        set_wait_cursor(FALSE);
        show_error(L"请先指定输出文件夹。");
        return;
    }

    GetLocalTime(&st);
    {
        wchar_t *wext = utf8_to_wide(ext, strlen(ext));
        fmtw(fname, 128, L"decoded_%04d%02d%02d_%02d%02d%02d.%s",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, wext);
        free(wext);
    }
    if (wcslen(g_outDir) + 1 + wcslen(fname) + 1 > PATHBUF) {
        free(data);
        set_wait_cursor(FALSE);
        set_status(g_lblDecStatus, &g_decStatusErr, 1, L"输出路径过长，未生成文件。");
        show_error(L"输出路径过长（上限 %d 个字符）。\n\n当前输出文件夹：\n%s\n\n"
                   L"请改选层级更浅的文件夹。", PATHBUF - 1, g_outDir);
        return;
    }
    fmtw(fullPath, PATHBUF, L"%s\\%s", g_outDir, fname);
    make_unique_path(fullPath, PATHBUF);

    ok = write_whole_file(fullPath, data, dlen, err, 512);
    free(data);
    set_wait_cursor(FALSE);

    if (!ok) {
        set_status(g_lblDecStatus, &g_decStatusErr, 1, err);
        show_error(L"%s", err);
        return;
    }

    wcsncpy(g_lastSaved, fullPath, PATHBUF - 1);
    g_lastSaved[PATHBUF - 1] = L'\0';

    fmtw(stat, 1024, L"✓ 已保存 %I64u 字节 → %s", (unsigned long long)dlen, fname);
    set_status(g_lblDecStatus, &g_decStatusErr, 0, stat);

    {
        wchar_t note[512] = L"";
        wchar_t msg[4096];
        if (ignored)
            fmtw(note, 512, L"\n\n注：已自动忽略 %I64u 个非 Base64 字符（空格、换行、全角符号等）。",
                 (unsigned long long)ignored);
        fmtw(msg, 4096,
             L"解码成功，文件已保存：\n\n%s\n\n"
             L"文件大小：%I64u 字节\n系统识别类型：%s%s\n\n是否打开所在文件夹？",
             fullPath, (unsigned long long)dlen, wdesc, note);
        if (MessageBoxW(g_hMain, msg, L"完成", MB_YESNO | MB_ICONINFORMATION) == IDYES)
            reveal_in_explorer(fullPath);
    }
}

/* ====================================================================
 * ③ 文本 -> SHA-256
 * ==================================================================== */

static void do_hash(void)
{
    wchar_t *txt;
    char *u8;
    size_t u8len = 0, i, o = 0;
    char hex[65];
    wchar_t stat[256];
    int upper = (SendMessageW(g_chkUpper, BM_GETCHECK, 0, 0) == BST_CHECKED);
    size_t chars;

    txt = get_edit_text(g_txtHashIn);
    if (!txt) {
        show_error(L"内存不足。");
        return;
    } 
    chars = wcslen(txt);

    u8 = wide_to_utf8(txt, &u8len);
    free(txt);
    if (!u8) {
        show_error(L"内存不足。");
        return;
    }

    /* 统一把 CRLF 归一为 LF，保证与其它平台的文本哈希一致 */
    for (i = 0; i < u8len; i++) {
        if (u8[i] == '\r' && i + 1 < u8len && u8[i + 1] == '\n')
            continue;
        u8[o++] = u8[i];
    }

    sha256_text(u8, o, hex, upper);
    free(u8);

    {
        wchar_t *w = utf8_to_wide(hex, 64);
        SetWindowTextW(g_txtHashOut, w);
        free(w);
    }

    if (upper)
        fmtw(stat, 256, L"已生成大写摘要　·　输入 %I64u 个字符 / %I64u 字节（UTF-8，换行按 LF 计）",
             (unsigned long long)chars, (unsigned long long)o);
    else
        fmtw(stat, 256, L"已生成摘要　·　输入 %I64u 个字符 / %I64u 字节（UTF-8，换行按 LF 计）",
             (unsigned long long)chars, (unsigned long long)o);

    if (o == 0)
        fmtw(stat, 256, L"输入为空，以上是空字符串的 SHA-256 摘要。");
    set_status(g_lblHashStatus, &g_hashStatusErr, 0, stat);
}

/* ====================================================================
 * 界面构建
 * ==================================================================== */

static LRESULT CALLBACK PanelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_COMMAND || msg == WM_NOTIFY || msg == WM_DROPFILES ||
        msg == WM_VSCROLL || msg == WM_MOUSEWHEEL)
        return SendMessageW(GetParent(h), msg, wp, lp);
    if (msg == WM_CTLCOLORSTATIC)
        return SendMessageW(GetParent(h), WM_CTLCOLORSTATIC, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

static void add_tab(int index, const wchar_t *text)
{
    TCITEMW tie;
    memset(&tie, 0, sizeof(tie));
    tie.mask = TCIF_TEXT;
    tie.pszText = (LPWSTR)text;
    SendMessageW(g_hTab, TCM_INSERTITEMW, (WPARAM)index, (LPARAM)&tie);
}

/* 供 apitest 取标准行高（它不直接访问 g_lineH） */
static int ui_line_height(void)
{
    return g_lineH;
}

static int build_ui(HWND parent)
{
    int i;

    /*
     * ============================ 这里曾经是空白界面的根因 ============================
     * 父窗口由调用方显式传入，绝不能再用全局 g_hMain。
     *
     * 原因：初始化代码在 WM_CREATE 里调用 build_ui()，而主窗口句柄的赋值语句是
     *       g_hMain = CreateWindowExW(...);   —— 要等 CreateWindowExW 返回之后才执行。
     *       也就是说 WM_CREATE 执行期间 g_hMain 仍然是 NULL。
     *       带 WS_CHILD 的窗口在创建时必须有一个有效的父窗口，父窗口为 NULL 时
     *       CreateWindowExW 直接失败返回 NULL。于是选项卡、三个面板、状态栏和全部
     *       输入框/按钮统统没被创建出来 —— 窗口壳子正常显示，里面一片空白。
     *
     * 修法：WM_CREATE 里用消息自带的 h（就是主窗口本身）作为父窗口；
     *       主窗口创建完成后再用 g_hMain 兜底构建一次，两者由 g_uiBuilt 去重。
     * =================================================================================
     */
    if (g_uiBuilt)
        return 0;                      /* 已经构建过，直接成功返回 */
    if (!parent || !IsWindow(parent)) {
        g_mkFails++;
        g_mkLastErr = ERROR_INVALID_WINDOW_HANDLE;
        wcsncpy(g_mkLastCls, L"(主窗口句柄无效)", 63);
        g_mkLastCls[63] = L'\0';
        return -1;
    }
    g_uiBuilt = 1;
    g_buildParent = parent;

    /* ---------- 选项卡 ---------- */
    g_hTab = CreateWindowExW(0, WC_TABCONTROLW, L"",
                             WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
                             0, 0, 100, 100, parent, (HMENU)ID_TAB, g_hInst, NULL);
    if (!g_hTab) {
        g_mkFails++;
        g_mkLastErr = GetLastError();
        wcsncpy(g_mkLastCls, WC_TABCONTROLW, 63);
        g_mkLastCls[63] = L'\0';
    }
    set_font(g_hTab, g_fontUI);
    add_tab(0, L"  ① 文件编码 Base64  ");
    add_tab(1, L"  ② 解码还原为文件  ");
    add_tab(2, L"  ③ 文本 SHA-256 摘要  ");
    add_tab(3, L"  ④ 接口测试  ");
    add_tab(4, L"  ⑤ 批量接口测试  ");

    for (i = 0; i < PANEL_COUNT; i++) {
        g_panel[i] = CreateWindowExW(WS_EX_CONTROLPARENT, L"B64Panel", L"",
                                     WS_CHILD | WS_CLIPCHILDREN,
                                     0, 0, 100, 100, parent, NULL, g_hInst, NULL);
        if (!g_panel[i]) {
            g_mkFails++;
            g_mkLastErr = GetLastError();
            wcsncpy(g_mkLastCls, L"B64Panel", 63);
            g_mkLastCls[63] = L'\0';
        }
        /* 面板也要接受拖拽：文件多半是直接丢到面板空白处 */
        if (g_panel[i])
            DragAcceptFiles(g_panel[i], TRUE);
    }
    ShowWindow(g_panel[0], SW_SHOW);

    g_hStatus = mk(parent, L"STATIC",
                   L"就绪。①②③ 在本机离线完成；④ 接口测试只在你点击发送时访问所填地址。"
                   L"（命令行加 /selftest /api-test 可运行自检）",
                   SS_LEFTNOWORDWRAP | SS_SUNKEN, 0, ID_STATUS, g_fontUI);

    /* ================= ① 编码面板 ================= */
    {
        HWND p = g_panel[0];
        g_lblSrc = mk(p, L"STATIC", L"源文件:", SS_LEFT, 0, ID_LBL_SRC, g_fontUI);
        g_edtSrc = mk(p, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP,
                      WS_EX_CLIENTEDGE, ID_EDT_SRC, g_fontUI);
        g_btnPick = mk(p, L"BUTTON", L"选择文件…", BS_PUSHBUTTON | WS_TABSTOP, 0,
                       ID_BTN_PICK, g_fontUI);
        g_btnEncode = mk(p, L"BUTTON", L"开始转换 → Base64", BS_PUSHBUTTON | WS_TABSTOP, 0,
                         ID_BTN_ENCODE, g_fontUI);
        g_btnCopyEnc = mk(p, L"BUTTON", L"复制结果", BS_PUSHBUTTON | WS_TABSTOP, 0,
                          ID_BTN_COPY_ENC, g_fontUI);
        g_btnSaveTxt = mk(p, L"BUTTON", L"保存为 TXT…", BS_PUSHBUTTON | WS_TABSTOP, 0,
                          ID_BTN_SAVE_TXT, g_fontUI);
        g_btnClearEnc = mk(p, L"BUTTON", L"清空", BS_PUSHBUTTON | WS_TABSTOP, 0,
                           ID_BTN_CLEAR_ENC, g_fontUI);
        g_chkWrap = mk(p, L"BUTTON", L"每 76 字符换行（MIME 标准）",
                       BS_AUTOCHECKBOX | WS_TABSTOP, 0, ID_CHK_WRAP, g_fontUI);
        SendMessageW(g_chkWrap, BM_SETCHECK, BST_CHECKED, 0);
        g_txtEnc = mk(p, L"EDIT", L"",
                      ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | WS_TABSTOP,
                      WS_EX_CLIENTEDGE, ID_TXT_ENC, g_fontMono);
        set_text_limit(g_txtEnc);
        g_lblEncStatus = mk(p, L"STATIC", L"", SS_LEFTNOWORDWRAP, 0, ID_LBL_ENC_STATUS, g_fontUI);
    }

    /* ================= ② 解码面板 ================= */
    {
        HWND p = g_panel[1];
        size_t n, k;

        g_lblDecHint = mk(p, L"STATIC",
                          L"粘贴 Base64 内容（自动忽略空格与换行；支持标准与 URL-safe 字母表）:",
                          SS_LEFT, 0, ID_LBL_DEC_HINT, g_fontUI);
        g_txtDec = mk(p, L"EDIT", L"",
                      ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP,
                      WS_EX_CLIENTEDGE, ID_TXT_DEC, g_fontMono);
        set_text_limit(g_txtDec);
        g_btnDecode = mk(p, L"BUTTON", L"解码并保存到文件夹", BS_PUSHBUTTON | WS_TABSTOP, 0,
                         ID_BTN_DECODE, g_fontUI);
        g_btnClearDec = mk(p, L"BUTTON", L"清空", BS_PUSHBUTTON | WS_TABSTOP, 0,
                           ID_BTN_CLEAR_DEC, g_fontUI);
        g_btnOpenDir = mk(p, L"BUTTON", L"打开输出文件夹", BS_PUSHBUTTON | WS_TABSTOP, 0,
                          ID_BTN_OPEN_DIR, g_fontUI);

        g_lblType = mk(p, L"STATIC", L"目标文件类型:", SS_LEFT, 0, ID_LBL_TYPE, g_fontUI);
        g_cmbType = mk(p, L"COMBOBOX", L"",
                       CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, ID_CMB_TYPE, g_fontUI);
        n = filetype_option_count();
        for (k = 0; k < n; k++) {
            const char *nm = filetype_option_name(k);
            const char *ex = filetype_option_ext(k);
            char item[192];
            wchar_t *w;
            if (ex && ex[0])
                snprintf(item, sizeof(item), "%s  (*.%s)", nm, ex);
            else
                snprintf(item, sizeof(item), "%s", nm);
            /* 注意：组合框是 Unicode 控件，必须传宽字符，否则中文会变成乱码 */
            w = utf8_to_wide(item, strlen(item));
            SendMessageW(g_cmbType, CB_ADDSTRING, 0, (LPARAM)w);
            free(w);
        }
        SendMessageW(g_cmbType, CB_SETCURSEL, 0, 0);

        g_lblOutDir = mk(p, L"STATIC", L"输出文件夹:", SS_LEFT, 0, ID_LBL_OUTDIR, g_fontUI);
        g_edtOutDir = mk(p, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP,
                         WS_EX_CLIENTEDGE, ID_EDT_OUTDIR, g_fontUI);
        g_btnOutDir = mk(p, L"BUTTON", L"浏览…", BS_PUSHBUTTON | WS_TABSTOP, 0,
                         ID_BTN_OUTDIR, g_fontUI);
        g_lblDecStatus = mk(p, L"STATIC", L"", SS_LEFTNOWORDWRAP, 0, ID_LBL_DEC_STATUS, g_fontUI);
    }

    /* ================= ③ 摘要面板 ================= */
    {
        HWND p = g_panel[2];
        g_lblHashHint = mk(p, L"STATIC",
                           L"输入文本（支持中文与多行；计算结果覆盖文本框内的全部字符）:",
                           SS_LEFT, 0, ID_LBL_HASH_HINT, g_fontUI);
        g_txtHashIn = mk(p, L"EDIT", L"",
                         ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP,
                         WS_EX_CLIENTEDGE, ID_TXT_HASH_IN, g_fontMono);
        set_text_limit(g_txtHashIn);
        g_btnHash = mk(p, L"BUTTON", L"生成 SHA-256 摘要", BS_PUSHBUTTON | WS_TABSTOP, 0,
                       ID_BTN_HASH, g_fontUI);
        g_btnClearHash = mk(p, L"BUTTON", L"清空", BS_PUSHBUTTON | WS_TABSTOP, 0,
                            ID_BTN_CLEAR_HASH, g_fontUI);
        g_btnCopyHash = mk(p, L"BUTTON", L"复制摘要", BS_PUSHBUTTON | WS_TABSTOP, 0,
                           ID_BTN_COPY_HASH, g_fontUI);
        g_chkUpper = mk(p, L"BUTTON", L"大写输出", BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                        ID_CHK_UPPER, g_fontUI);
        g_lblHashOut = mk(p, L"STATIC", L"SHA-256 摘要（64 位十六进制）:", SS_LEFT, 0,
                          ID_LBL_HASH_OUT, g_fontUI);
        g_txtHashOut = mk(p, L"EDIT", L"",
                          ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | WS_TABSTOP,
                          WS_EX_CLIENTEDGE, ID_TXT_HASH_OUT, g_fontMono);
        set_text_limit(g_txtHashOut);
        g_lblHashStatus = mk(p, L"STATIC", L"", SS_LEFTNOWORDWRAP, 0,
                             ID_LBL_HASH_STATUS, g_fontUI);
    }

    /* ================= ④ 接口测试面板 ================= */
    {
        api_ui_ctx uic;

        memset(&uic, 0, sizeof(uic));
        uic.inst = g_hInst;
        uic.mk = mk;
        uic.ds = ds;
        uic.line_h = ui_line_height;
        uic.font_ui = g_fontUI;
        uic.font_mono = g_fontMono;
        uic.br_panel = g_brPanel;
        uic.pick_file = pick_open_file;
        uic.pick_save_txt = pick_save_txt;
        uic.pick_folder = pick_folder;
        uic.read_file = read_whole_file;
        uic.write_file = write_whole_file;
        uic.unique_path = make_unique_path;
        apitest_build(g_panel[3], &uic);
        batchtest_build(g_panel[4], &uic);
    }

    /* 选项卡必须停在同级窗口的最底层，否则会盖住叠在它上面的面板 */
    if (g_hTab)
        SetWindowPos(g_hTab, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    return g_mkFails;
}

/* 界面建好之后的初始状态（默认输出目录、各区域提示文字、默认选项卡） */
static void init_ui_state(void)
{
    init_default_outdir();
    SetWindowTextW(g_edtOutDir, g_outDir);
    update_src_info();
    set_status(g_lblDecStatus, &g_decStatusErr, 0,
               L"默认「自动判断」文件类型；解码文件将保存到上面的文件夹。");
    set_status(g_lblHashStatus, &g_hashStatusErr, 0,
               L"输入文本后点击「生成 SHA-256 摘要」（输入框内容变化时会自动刷新）。");
    SetWindowTextW(g_txtHashOut, L"");
    apitest_init_state();
    batchtest_init_state();
    switch_tab(0);      /* 统一初始状态：显示面板①、摆好全部控件、选项卡压到最底层 */
}

/* 各面板内部排版 ------------------------------------------------------ */

static void layout_panel1(int px, int py, int pw, int ph)
{
    int m = ds(12), gap = ds(8), rh = g_lineH;
    int x0 = px + m, iw = pw - 2 * m;
    int y = py + m;
    int btnH = rh;
    int statusY = py + ph - m - rh;
    int boxTop, boxH;
    int lw = ds(64), bw = ds(110);

    MoveWindow(g_lblSrc, x0, y + ds(3), lw, rh, TRUE);
    MoveWindow(g_edtSrc, x0 + lw, y, iw - lw - bw - gap, btnH, TRUE);
    MoveWindow(g_btnPick, x0 + iw - bw, y, bw, btnH, TRUE);

    y += btnH + gap;
    {
        int w1 = ds(168), w2 = ds(100), w3 = ds(120), w4 = ds(80), x = x0;
        MoveWindow(g_btnEncode, x, y, w1, btnH, TRUE); x += w1 + gap;
        MoveWindow(g_btnCopyEnc, x, y, w2, btnH, TRUE); x += w2 + gap;
        MoveWindow(g_btnSaveTxt, x, y, w3, btnH, TRUE); x += w3 + gap;
        MoveWindow(g_btnClearEnc, x, y, w4, btnH, TRUE); x += w4 + gap * 2;
        MoveWindow(g_chkWrap, x, y + ds(3), ds(230), rh, TRUE);
    }

    y += btnH + ds(6);
    boxTop = y;
    boxH = statusY - ds(8) - boxTop;
    if (boxH < ds(120))
        boxH = ds(120);
    MoveWindow(g_txtEnc, x0, boxTop, iw, boxH, TRUE);
    MoveWindow(g_lblEncStatus, x0, statusY, iw, rh, TRUE);
}

static void layout_panel2(int px, int py, int pw, int ph)
{
    int m = ds(12), gap = ds(8), rh = g_lineH;
    int x0 = px + m, iw = pw - 2 * m;
    int y = py + m;
    int btnH = rh;
    int statusY = py + ph - m - rh;
    int rowB = statusY - gap - btnH;
    int rowA = rowB - gap - btnH;
    int boxTop, boxH;

    MoveWindow(g_lblDecHint, x0, y, iw, rh, TRUE);
    y += rh + ds(4);
    boxTop = y;
    boxH = rowA - ds(8) - boxTop;
    if (boxH < ds(120))
        boxH = ds(120);
    MoveWindow(g_txtDec, x0, boxTop, iw, boxH, TRUE);

    /* 行 A：解码按钮 + 清空 + 打开文件夹 */
    {
        int w1 = ds(190), w2 = ds(80), w3 = ds(150), x = x0;
        MoveWindow(g_btnDecode, x, rowA, w1, btnH, TRUE); x += w1 + gap;
        MoveWindow(g_btnClearDec, x, rowA, w2, btnH, TRUE); x += w2 + gap;
        MoveWindow(g_btnOpenDir, x, rowA, w3, btnH, TRUE);
    }

    /* 行 B：目标类型 + 输出文件夹 + 浏览 */
    {
        int lw1 = ds(84), cw = ds(230), lw2 = ds(76), bw = ds(80);
        int x = x0;
        MoveWindow(g_lblType, x, rowB + ds(4), lw1, rh, TRUE);
        x += lw1;
        MoveWindow(g_cmbType, x, rowB, cw, rh + ds(180), TRUE);
        x += cw + gap * 2;
        MoveWindow(g_lblOutDir, x, rowB + ds(4), lw2, rh, TRUE);
        x += lw2;
        MoveWindow(g_edtOutDir, x, rowB, x0 + iw - bw - gap - x, btnH, TRUE);
        MoveWindow(g_btnOutDir, x0 + iw - bw, rowB, bw, btnH, TRUE);
    }

    MoveWindow(g_lblDecStatus, x0, statusY, iw, rh, TRUE);
}

static void layout_panel3(int px, int py, int pw, int ph)
{
    int m = ds(12), gap = ds(8), rh = g_lineH;
    int x0 = px + m, iw = pw - 2 * m;
    int y = py + m;
    int btnH = rh;
    int statusY = py + ph - m - rh;
    int rowA = statusY - gap - btnH;
    int inTop, inH, outLabelY, outTop, outH;

    MoveWindow(g_lblHashHint, x0, y, iw, rh, TRUE);
    y += rh + ds(4);

    inTop = y;
    inH = (rowA - ds(10) - inTop) * 45 / 100;
    if (inH < ds(90))
        inH = ds(90);
    MoveWindow(g_txtHashIn, x0, inTop, iw, inH, TRUE);

    MoveWindow(g_btnHash, x0, rowA, ds(180), btnH, TRUE);
    MoveWindow(g_btnClearHash, x0 + ds(180) + gap, rowA, ds(80), btnH, TRUE);
    MoveWindow(g_btnCopyHash, x0 + ds(180) + gap * 2 + ds(80), rowA, ds(100), btnH, TRUE);
    MoveWindow(g_chkUpper, x0 + ds(180) + gap * 3 + ds(180), rowA + ds(3), ds(110), rh, TRUE);

    outLabelY = inTop + inH + ds(10);
    MoveWindow(g_lblHashOut, x0, outLabelY, iw, rh, TRUE);

    outTop = outLabelY + rh + ds(4);
    outH = rowA - ds(8) - outTop;
    if (outH < ds(60))
        outH = ds(60);
    MoveWindow(g_txtHashOut, x0, outTop, iw, outH, TRUE);

    MoveWindow(g_lblHashStatus, x0, statusY, iw, rh, TRUE);
}

static void layout(void)
{
    RECT rc, tr;
    int W, H, sbH, px, py, pw, ph, i;

    /* 主窗口尚未创建（或已销毁）时不布局，避免读到未初始化的矩形 */
    if (!g_hMain || !IsWindow(g_hMain))
        return;

    GetClientRect(g_hMain, &rc);
    W = rc.right;
    H = rc.bottom;
    sbH = g_lineH + ds(6);

    /* 窗口还没拿到真实尺寸（最小化、尚未显示）时不布局，等下一次尺寸变化 */
    if (W < ds(220) || H < sbH + ds(220))
        return;

    MoveWindow(g_hTab, 0, 0, W, H - sbH, TRUE);
    MoveWindow(g_hStatus, 0, H - sbH, W, sbH, TRUE);

    /*
     * 取选项卡的"内容显示区"（页签那一行以下的部分）。
     *   TCM_ADJUSTRECT 的 wParam = TRUE  -> 传入选项卡窗口矩形，返回内容显示区
     *                              FALSE -> 传入内容显示区，返回窗口矩形
     * 这里必须用 TRUE。此前误用 FALSE，拿到的是向上越界的负坐标，
     * 会把面板顶到页签上去（页签既看不见也点不动）。
     * 选项卡刚被放在主窗口客户区 (0,0)，因此返回的坐标可直接当作主窗口客户区坐标。
     *
     * 先给一套保守的兜底值，再用消息返回的真实值覆盖：即使某条消息行为异常，
     * 面板也一定落在页签下方、不会盖住页签，界面一定能出来。
     */
    px = 2;
    py = ds(26);
    pw = W - 4;
    ph = H - sbH - py - 2;
    if (g_hTab && IsWindow(g_hTab)) {
        RECT ir;

        /* 兜底：用第一个页签的下边界推算内容区起点 */
        if (SendMessageW(g_hTab, TCM_GETITEMRECT, 0, (LPARAM)&ir)) {
            py = ir.bottom + ds(4);
            ph = H - sbH - py - 2;
        }

        GetClientRect(g_hTab, &tr);
        SendMessageW(g_hTab, TCM_ADJUSTRECT, TRUE, (LPARAM)&tr);
        if (tr.left >= 0 && tr.top > 0 &&
            tr.right > tr.left && tr.bottom > tr.top) {
            px = tr.left;
            py = tr.top;
            pw = tr.right - tr.left;
            ph = tr.bottom - tr.top;
        }
    }
    if (pw > W - px)
        pw = W - px;
    if (ph > H - sbH - py)
        ph = H - sbH - py;
    if (pw < 1 || ph < 1)
        return;

    /*
     * 三个面板摆到完全相同的位置与尺寸，切换时只显示/隐藏其中一个。
     * 关键：面板自己必须 MoveWindow —— 此前只移动了面板里的控件，面板本身还停在
     * 创建时的 100x100，子控件会被父窗口裁剪掉，界面看上去就是一片空白。
     */
    for (i = 0; i < PANEL_COUNT; i++) {
        if (g_panel[i])
            MoveWindow(g_panel[i], px, py, pw, ph, TRUE);
    }

    /* 面板内的控件用"面板坐标系"（原点在面板左上角），所以这里传 0,0 */
    layout_panel1(0, 0, pw, ph);
    layout_panel2(0, 0, pw, ph);
    layout_panel3(0, 0, pw, ph);
    apitest_layout(0, 0, pw, ph);
    batchtest_layout(0, 0, pw, ph);
}

static void switch_tab(int index)
{
    int i;
    if (index < 0 || index >= PANEL_COUNT)
        index = 0;
    for (i = 0; i < PANEL_COUNT; i++)
        ShowWindow(g_panel[i], (i == index) ? SW_SHOW : SW_HIDE);

    /* 面板显示后可能跑到选项卡前面去：把选项卡重新压到底层，保证页签始终露在上层且可点击 */
    if (g_hTab)
        SetWindowPos(g_hTab, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    layout();

    /*
     * 把焦点带进新面板：否则焦点可能停留在刚被隐藏的控件上，
     * 用户继续敲键盘就会"打字打进了看不见的地方"。
     * 窗口尚未显示时（构建期）不动焦点，避免抢其它程序的激活状态。
     */
    if (IsWindowVisible(g_hMain)) {
        HWND target = (index == 0) ? g_btnPick
                    : (index == 1) ? g_txtDec
                    : (index == 2) ? g_txtHashIn
                                   : NULL;
        if (index == 3)
            apitest_focus_target();
        else if (target)
            SetFocus(target);
    }
}

/* ====================================================================
 * 消息处理
 * ==================================================================== */

static void init_default_outdir(void)
{
    wchar_t p[PATHBUF];

    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, p))) {
        wcsncpy(g_outDir, p, PATHBUF - 1);
        g_outDir[PATHBUF - 1] = L'\0';
        return;
    }
    GetModuleFileNameW(NULL, p, PATHBUF);
    {
        wchar_t *slash = wcsrchr(p, L'\\');
        if (slash) {
            *(slash + 1) = L'\0';
            wcsncpy(g_outDir, p, PATHBUF - 1);
            g_outDir[PATHBUF - 1] = L'\0';
        }
    }
}

static void clear_encode(void)
{
    free(g_b64);
    g_b64 = NULL;
    g_b64Len = 0;
    g_srcPath[0] = L'\0';
    g_srcSize = 0;
    SetWindowTextW(g_edtSrc, L"");
    SetWindowTextW(g_txtEnc, L"");
    update_src_info();
}

static LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        /*
         * 用消息自带的 h 作为父窗口来构建界面 —— 它就是主窗口句柄本身。
         * 切忌在这里使用全局 g_hMain：此刻 CreateWindowExW 尚未返回，g_hMain 还是 NULL，
         * 而带 WS_CHILD 的窗口没有有效父窗口就创建不出来，结果就是一片空白。
         */
        build_ui(h);
        DragAcceptFiles(h, TRUE);
        return 0;

    case WM_SIZE:
        layout();
        return 0;

    case WM_GETMINMAXINFO:
        {
            MINMAXINFO *mmi = (MINMAXINFO *)lp;
            mmi->ptMinTrackSize.x = ds(860);
            mmi->ptMinTrackSize.y = ds(660);
        }
        return 0;

    case WM_NOTIFY:
        {
            NMHDR *nm = (NMHDR *)lp;
            if (nm->idFrom == ID_TAB && nm->code == TCN_SELCHANGE) {
                int cur = (int)SendMessageW(g_hTab, TCM_GETCURSEL, 0, 0);
                switch_tab(cur);
                return 0;
            }
            /* 第 5 页的表格（ListView）通知 */
            if (batchtest_on_notify(nm))
                return 0;
        }
        return 0;

    case WM_DROPFILES:
        {
            HDROP drop = (HDROP)wp;

            /* 当前停在第 4 页时，拖进来的文件应该成为接口测试的源文件 */
            if (SendMessageW(g_hTab, TCM_GETCURSEL, 0, 0) == 3) {
                apitest_on_dropfiles(drop);
                return 0;
            }
            /* 第 5 页按文件夹工作，拖进来的单个文件没有意义 */
            if (SendMessageW(g_hTab, TCM_GETCURSEL, 0, 0) == 4) {
                DragFinish(drop);
                MessageBoxW(h, L"批量页需要的是文件夹，请用「选择文件夹…」。",
                            L"批量接口测试", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            {
                wchar_t path[PATHBUF];
                UINT got = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
                if (got > 0) {
                    DragQueryFileW(drop, 0, path, PATHBUF);
                    DragFinish(drop);
                    SendMessageW(g_hTab, TCM_SETCURSEL, 0, 0);
                    switch_tab(0);
                    set_source_file(path);
                } else {
                    DragFinish(drop);
                }
            }
        }
        return 0;

    case WM_CTLCOLORSTATIC:
        {
            HWND ctl = (HWND)lp;
            wchar_t cls[32];
            int isErr = 0, isStatus = 0;

            /* 第 4 页的 meta1/meta2/hint 由 apitest 自己上色（错误红/提示灰） */
            {
                LRESULT lr = 0;
                if (apitest_ctlcolor(ctl, (HDC)wp, &lr))
                    return lr;
            }

            /* 底部状态条 */
            if (ctl == g_hStatus) {
                SetBkMode((HDC)wp, OPAQUE);
                SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
                SetTextColor((HDC)wp, GetSysColor(COLOR_BTNTEXT));
                return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
            }

            /*
             * 只读的 EDIT / COMBOBOX 也会发 WM_CTLCOLORSTATIC，
             * 这里必须返回窗口色画笔，否则输入区会被涂成按钮灰。
             */
            cls[0] = L'\0';
            GetClassNameW(ctl, cls, 31);
            if (cls[0] && (_wcsicmp(cls, L"Edit") == 0 || _wcsicmp(cls, L"ComboBox") == 0)) {
                SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
                SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
                return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
            }

            if (ctl == g_lblEncStatus) { isStatus = 1; isErr = g_encStatusErr; }
            else if (ctl == g_lblDecStatus) { isStatus = 1; isErr = g_decStatusErr; }
            else if (ctl == g_lblHashStatus) { isStatus = 1; isErr = g_hashStatusErr; }

            SetBkMode((HDC)wp, TRANSPARENT);
            if (isStatus)
                SetTextColor((HDC)wp, isErr ? RGB(180, 24, 24) : RGB(40, 90, 40));
            else if (ctl == g_lblDecHint || ctl == g_lblHashHint)
                SetTextColor((HDC)wp, RGB(70, 70, 70));
            else
                SetTextColor((HDC)wp, GetSysColor(COLOR_BTNTEXT));
            return (LRESULT)g_brPanel;
        }

    case WM_COMMAND:
        /* 第 4 页的控件 ID 独立号段，先让 apitest 处理，未命中再往下走 */
        if (batchtest_on_command(h, LOWORD(wp), HIWORD(wp)))
            return 0;
        if (apitest_on_command(h, LOWORD(wp), HIWORD(wp)))
            return 0;
        switch (LOWORD(wp)) {
        /* ---------- ① 编码 ---------- */
        case ID_BTN_PICK:
            {
                wchar_t path[PATHBUF];
                if (pick_open_file(path, PATHBUF))
                    set_source_file(path);
            }
            return 0;
        case ID_BTN_ENCODE:
            do_encode();
            return 0;
        case ID_BTN_COPY_ENC:
            if (!g_b64 || g_b64Len == 0) {
                show_error(L"还没有可复制的内容，请先完成一次转换。");
                return 0;
            }
            {
                size_t n;
                char *t = current_b64_text(&n);
                copy_text_to_clipboard(t, n);
                free(t);
                set_status(g_lblEncStatus, &g_encStatusErr, 0,
                           L"✓ Base64 内容已复制到剪贴板。");
            }
            return 0;
        case ID_BTN_SAVE_TXT:
            if (!g_b64 || g_b64Len == 0) {
                show_error(L"还没有可保存的内容，请先完成一次转换。");
                return 0;
            }
            {
                wchar_t path[PATHBUF], err[512];
                size_t n;
                char *t;
                if (!pick_save_txt(path, PATHBUF, L"base64_output.txt"))
                    return 0;
                t = current_b64_text(&n);
                set_wait_cursor(TRUE);
                if (write_whole_file(path, t, n, err, 512)) {
                    set_status(g_lblEncStatus, &g_encStatusErr, 0,
                               L"✓ Base64 文本已保存。");
                    set_wait_cursor(FALSE);
                    show_info(L"Base64 文本已保存到：\n\n%s", path);
                } else {
                    set_wait_cursor(FALSE);
                    set_status(g_lblEncStatus, &g_encStatusErr, 1, err);
                    show_error(L"%s", err);
                }
                free(t);
            }
            return 0;
        case ID_BTN_CLEAR_ENC:
            clear_encode();
            return 0;
        case ID_CHK_WRAP:
            render_b64_to_box();
            return 0;

        /* ---------- ② 解码 ---------- */
        case ID_BTN_DECODE:
            do_decode();
            return 0;
        case ID_BTN_CLEAR_DEC:
            SetWindowTextW(g_txtDec, L"");
            set_status(g_lblDecStatus, &g_decStatusErr, 0, L"已清空输入框。");
            return 0;
        case ID_BTN_OUTDIR:
            {
                wchar_t dir[PATHBUF];
                if (pick_folder(h, L"选择解码文件的保存文件夹", dir, PATHBUF)) {
                    wcsncpy(g_outDir, dir, PATHBUF - 1);
                    g_outDir[PATHBUF - 1] = L'\0';
                    SetWindowTextW(g_edtOutDir, g_outDir);
                }
            }
            return 0;
        case ID_BTN_OPEN_DIR:
            if (g_lastSaved[0] && GetFileAttributesW(g_lastSaved) != INVALID_FILE_ATTRIBUTES)
                reveal_in_explorer(g_lastSaved);
            else if (g_outDir[0])
                open_folder(g_outDir);
            else
                show_error(L"尚未指定输出文件夹。");
            return 0;
        case ID_CMB_TYPE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(g_cmbType, CB_GETCURSEL, 0, 0);
                const char *e = filetype_option_ext((size_t)sel);
                if (!e || e[0] == '\0')
                    set_status(g_lblDecStatus, &g_decStatusErr, 0,
                               L"已选择「自动判断」：解码后按文件内容自动识别真实类型。");
                else {
                    wchar_t *w = utf8_to_wide(e, strlen(e));
                    wchar_t s[256];
                    fmtw(s, 256, L"已指定目标类型：*.%s", w);
                    set_status(g_lblDecStatus, &g_decStatusErr, 0, s);
                    free(w);
                }
            }
            return 0;

        /* ---------- ③ 摘要 ---------- */
        case ID_BTN_HASH:
            do_hash();
            return 0;
        case ID_BTN_CLEAR_HASH:
            g_hashUpdating = 1;
            SetWindowTextW(g_txtHashIn, L"");
            SetWindowTextW(g_txtHashOut, L"");
            g_hashUpdating = 0;
            set_status(g_lblHashStatus, &g_hashStatusErr, 0, L"已清空。");
            return 0;
        case ID_BTN_COPY_HASH:
            {
                wchar_t *w = get_edit_text(g_txtHashOut);
                if (!w || !w[0]) {
                    free(w);
                    show_error(L"还没有生成摘要。");
                    return 0;
                }
                {
                    size_t n;
                    char *u = wide_to_utf8(w, &n);
                    copy_text_to_clipboard(u, n);
                    free(u);
                }
                free(w);
                set_status(g_lblHashStatus, &g_hashStatusErr, 0, L"✓ 摘要已复制到剪贴板。");
            }
            return 0;
        case ID_CHK_UPPER:
            do_hash();
            return 0;
        case ID_TXT_HASH_IN:
            if (HIWORD(wp) == EN_CHANGE && !g_hashUpdating)
                do_hash();
            return 0;
        }
        return 0;

    /* 接口测试请求完成（worker 线程 PostMessage 回来） */
    case WM_API_DONE:
        apitest_on_done((UINT_PTR)wp, lp);
        return 0;

    /* 批量：某个文件返回 / 整批结束 */
    case WM_BATCH_ROW:
        batchtest_on_row((int)wp, (api_result *)lp);
        return 0;

    case WM_BATCH_DONE:
        batchtest_on_finished();
        return 0;

    /*
     * 第 4 页参数区的滚动条。
     * 编辑框/组合框的滚动由控件自己处理，不会把 WM_VSCROLL 抛给父窗口；
     * 能走到这里的只有 apitest 建的那个 SCROLLBAR 控件。
     */
    case WM_VSCROLL:
        if (SendMessageW(g_hTab, TCM_GETCURSEL, 0, 0) == 4)
            batchtest_on_vscroll(wp);
        else
            apitest_on_vscroll(wp);
        return 0;

    /* 第 4 页参数区的滚轮 */
    case WM_MOUSEWHEEL:
        if (SendMessageW(g_hTab, TCM_GETCURSEL, 0, 0) == 4) {
            if (batchtest_wants_wheel()) {
                batchtest_on_wheel(GET_WHEEL_DELTA_WPARAM(wp));
                return 0;
            }
        } else if (apitest_wants_wheel()) {
            apitest_on_wheel(GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        }
        break;

    case WM_CLOSE:
        /* 请求还在跑时先确认：worker 不会碰窗口，但直接退出会丢掉结果 */
        if (batchtest_is_busy()) {
            if (MessageBoxW(h,
                            L"批量请求仍在进行中。\n\n"
                            L"现在退出会丢弃已收到的结果，确定退出吗？",
                            L"批量接口测试", MB_YESNO | MB_ICONWARNING) != IDYES)
                return 0;
        }
        if (apitest_is_busy()) {
            if (MessageBoxW(h,
                            L"请求仍在进行中（最长等于超时时间）。\n\n"
                            L"现在退出会丢弃这次结果，确定退出吗？",
                            L"接口测试", MB_YESNO | MB_ICONWARNING) != IDYES)
                return 0;
        }
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        apitest_on_close();
        batchtest_on_close();
        free(g_b64);
        g_b64 = NULL;
        if (g_fontUI) DeleteObject(g_fontUI);
        if (g_fontMono) DeleteObject(g_fontMono);
        if (g_brPanel) DeleteObject(g_brPanel);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ====================================================================
 * 内置自检（命令行 /selftest）
 * 用于在离线目标机上当场验证算法实现是否正常。
 * ==================================================================== */

/* 取命令行中 argv[0] 之后的部分（argv[0] 可能带引号，也可能不带） */
static const wchar_t *cmdline_args(void)
{
    const wchar_t *cl = GetCommandLineW();
    const wchar_t *args;

    if (!cl)
        return NULL;
    args = cl;
    if (*args == L'"') {
        args = wcschr(args + 1, L'"');
        if (args)
            args++;
    } else {
        while (*args && *args != L' ' && *args != L'\t')
            args++;
    }
    return (args && *args) ? args : NULL;
}

static int handle_selftest(void)
{
    const wchar_t *args = cmdline_args();
    char *report;
    int fails;
    wchar_t *w, title[160];

    if (!args)
        return 0;
    if (!wcsstr(args, L"/selftest") && !wcsstr(args, L"-selftest"))
        return 0;

    report = (char *)malloc(65536);
    if (!report) {
        MessageBoxW(NULL, L"内存不足，无法运行自检。", L"自检", MB_OK | MB_ICONERROR);
        return 1;
    }

    fails = b64sha_selftest(report, 65536);
    w = utf8_to_wide(report, strlen(report));
    fmtw(title, 160, fails ? L"内置自检未通过（%d 项失败）" : L"内置自检全部通过",
         fails);
    MessageBoxW(NULL, w, title, MB_OK | (fails ? MB_ICONERROR : MB_ICONINFORMATION));
    copy_text_to_clipboard(report, strlen(report));
    free(w);
    free(report);
    return 1;
}

/* ====================================================================
 * 界面几何自检（命令行 /diag）
 * 界面出不来时，把主窗口客户区、选项卡显示区、每个面板与关键控件的真实
 * 位置尺寸回显出来，便于远程定位是"没布局"还是"控件被裁掉了"。
 * ==================================================================== */

static void diag_append(wchar_t *buf, size_t cch, const wchar_t *name, HWND h)
{
    wchar_t tmp[256];
    size_t used = wcslen(buf);
    int x = -1, y = -1, w = -1, ht = -1, vis = -1;

    if (h && IsWindow(h)) {
        RECT r;
        POINT pts[2];
        if (GetWindowRect(h, &r)) {
            pts[0].x = r.left;   pts[0].y = r.top;
            pts[1].x = r.right;  pts[1].y = r.bottom;
            if (g_hMain && IsWindow(g_hMain))
                MapWindowPoints(NULL, g_hMain, pts, 2);
            x = (int)pts[0].x;
            y = (int)pts[0].y;
            w = (int)(pts[1].x - pts[0].x);
            ht = (int)(pts[1].y - pts[0].y);
        }
        vis = IsWindowVisible(h) ? 1 : 0;
    }

    if (!h || !IsWindow(h))
        fmtw(tmp, 256, L"%-14s <未创建>\r\n", name);
    else if (vis < 0)
        fmtw(tmp, 256, L"%-14s <取矩形失败>\r\n", name);
    else
        fmtw(tmp, 256, L"%-14s (%d,%d) %dx%d 可见=%d\r\n",
             name, x, y, w, ht, vis);

    if (used + wcslen(tmp) + 1 < cch)
        wcsncat(buf, tmp, cch - used - 1);
}

static void diag_panels(wchar_t *buf, size_t cch)
{
    diag_append(buf, cch, L"tab", g_hTab);
    diag_append(buf, cch, L"status", g_hStatus);
    diag_append(buf, cch, L"panel[0]", g_panel[0]);
    diag_append(buf, cch, L"panel[1]", g_panel[1]);
    diag_append(buf, cch, L"panel[2]", g_panel[2]);
    diag_append(buf, cch, L"panel[3]", g_panel[3]);
}

static void diag_panel_ctl(wchar_t *buf, size_t cch, int idx)
{
    if (idx == 0) {
        diag_append(buf, cch, L"src标签", g_lblSrc);
        diag_append(buf, cch, L"源文件框", g_edtSrc);
        diag_append(buf, cch, L"选择文件", g_btnPick);
        diag_append(buf, cch, L"开始转换", g_btnEncode);
        diag_append(buf, cch, L"BASE64框", g_txtEnc);
        diag_append(buf, cch, L"编码状态", g_lblEncStatus);
    } else if (idx == 1) {
        diag_append(buf, cch, L"提示标签", g_lblDecHint);
        diag_append(buf, cch, L"输入框", g_txtDec);
        diag_append(buf, cch, L"解码按钮", g_btnDecode);
        diag_append(buf, cch, L"类型下拉", g_cmbType);
        diag_append(buf, cch, L"输出文件夹", g_edtOutDir);
        diag_append(buf, cch, L"解码状态", g_lblDecStatus);
    } else {
        diag_append(buf, cch, L"提示标签", g_lblHashHint);
        diag_append(buf, cch, L"文本输入", g_txtHashIn);
        diag_append(buf, cch, L"摘要按钮", g_btnHash);
        diag_append(buf, cch, L"摘要输出", g_txtHashOut);
        diag_append(buf, cch, L"摘要状态", g_lblHashStatus);
    }
}

/* 控件矩形是否有效：非空、且起点不越出父窗口客户区 */
static int diag_rect_ok(HWND h, HWND parent, int *outW, int *outH)
{
    RECT r;
    POINT pts[2];

    if (outW) *outW = 0;
    if (outH) *outH = 0;
    if (!h || !IsWindow(h) || !parent || !IsWindow(parent))
        return 0;
    if (!GetWindowRect(h, &r))
        return 0;
    pts[0].x = r.left;  pts[0].y = r.top;
    pts[1].x = r.right; pts[1].y = r.bottom;
    MapWindowPoints(NULL, parent, pts, 2);
    if (outW) *outW = (int)(pts[1].x - pts[0].x);
    if (outH) *outH = (int)(pts[1].y - pts[0].y);
    if (pts[1].x - pts[0].x <= 0 || pts[1].y - pts[0].y <= 0)
        return 0;                       /* 尺寸为零 —— 会被裁掉，画不出来 */
    if (pts[0].x < 0 || pts[0].y < 0)
        return 0;                       /* 起点越界 —— 被推到父窗口外面 */
    return 1;
}

static int handle_diag(void)
{
    const wchar_t *args = cmdline_args();
    wchar_t *buf;
    RECT rc, tr;
    int t;

    if (!args || !wcsstr(args, L"/diag"))
        return 0;

    buf = (wchar_t *)calloc(32768, sizeof(wchar_t));
    if (!buf)
        return 1;

    {
        wchar_t hdr[2048];
        RECT wr;
        int wx = 0, wy = 0, ww = 0, wh = 0;

        GetClientRect(g_hMain, &rc);
        if (GetWindowRect(g_hMain, &wr)) {
            wx = (int)wr.left;
            wy = (int)wr.top;
            ww = (int)(wr.right - wr.left);
            wh = (int)(wr.bottom - wr.top);
        }
        fmtw(hdr, 2048,
             L"[界面几何自检]\r\n"
             L"主窗口客户区 %dx%d   DPI=%d   g_lineH=%d\r\n"
             L"主窗口屏幕位置 (%d,%d) %dx%d   可见=%d   最小化=%d\r\n"
             L"界面已构建=%d   主窗口=%p   构建时父窗口=%p   两者一致=%d\r\n"
             L"控件创建失败 %d 次   最后错误码=%lu   最后失败的控件=%s\r\n\r\n",
             (int)rc.right, (int)rc.bottom, g_dpi, g_lineH,
             wx, wy, ww, wh,
             IsWindowVisible(g_hMain) ? 1 : 0,
             IsIconic(g_hMain) ? 1 : 0,
             g_uiBuilt, (void *)g_hMain, (void *)g_buildParent,
             (g_buildParent == g_hMain) ? 1 : 0,
             g_mkFails, (unsigned long)g_mkLastErr,
             g_mkLastCls[0] ? g_mkLastCls : L"(无)");
        wcsncat(buf, hdr, 32767);
    }

    diag_panels(buf, 32768);

    /* 选项卡内容显示区（TCM_ADJUSTRECT 正确用法：wParam=TRUE） */
    tr.left = tr.top = tr.right = tr.bottom = 0;
    if (g_hTab && IsWindow(g_hTab)) {
        GetClientRect(g_hTab, &tr);
        SendMessageW(g_hTab, TCM_ADJUSTRECT, TRUE, (LPARAM)&tr);
    }
    {
        wchar_t l[256];
        fmtw(l, 256, L"\r\n显示区(相对主窗口客户区) (%d,%d) %dx%d\r\n",
             (int)tr.left, (int)tr.top,
             (int)(tr.right - tr.left), (int)(tr.bottom - tr.top));
        if (wcslen(buf) + wcslen(l) + 1 < 32768)
            wcsncat(buf, l, 32768 - wcslen(buf) - 1);
    }

    for (t = 0; t < PANEL_COUNT; t++) {
        wchar_t l[128];
        fmtw(l, 128, L"\r\n--- 面板%d 内控件 ---\r\n", t);
        if (wcslen(buf) + wcslen(l) + 1 < 32768)
            wcsncat(buf, l, 32768 - wcslen(buf) - 1);
        diag_panel_ctl(buf, 32768, t);
    }

    /* 布局校验：尺寸为零或起点越界的控件会被父窗口裁掉，直接判为不正常 */
    {
        int bad = 0, pw = 0, ph = 0;
        wchar_t l[512];

        bad += !diag_rect_ok(g_hTab, g_hMain, NULL, NULL);
        bad += !diag_rect_ok(g_hStatus, g_hMain, NULL, NULL);
        bad += !diag_rect_ok(g_panel[0], g_hMain, &pw, &ph);
        bad += !diag_rect_ok(g_panel[1], g_hMain, NULL, NULL);
        bad += !diag_rect_ok(g_panel[2], g_hMain, NULL, NULL);
        bad += !diag_rect_ok(g_panel[3], g_hMain, NULL, NULL);
        bad += !diag_rect_ok(g_lblSrc, g_panel[0], NULL, NULL);
        bad += !diag_rect_ok(g_edtSrc, g_panel[0], NULL, NULL);
        bad += !diag_rect_ok(g_btnPick, g_panel[0], NULL, NULL);
        bad += !diag_rect_ok(g_btnEncode, g_panel[0], NULL, NULL);
        bad += !diag_rect_ok(g_txtEnc, g_panel[0], NULL, NULL);
        bad += !diag_rect_ok(g_cmbType, g_panel[1], NULL, NULL);
        bad += !diag_rect_ok(g_txtDec, g_panel[1], NULL, NULL);
        bad += !diag_rect_ok(g_txtHashIn, g_panel[2], NULL, NULL);
        bad += !diag_rect_ok(g_txtHashOut, g_panel[2], NULL, NULL);

        apitest_diag(buf, 32768);
        batchtest_diag(buf, 32768);

        fmtw(l, 512,
             L"\r\n布局校验: %s\r\n"
             L"  面板内容区 %dx%d   选项卡可见=%d   状态栏可见=%d\r\n",
             bad ? L"未通过（有控件尺寸为零或越界，见上方坐标）"
                 : L"通过 —— 界面应当正常可见",
             pw, ph,
             (g_hTab && IsWindowVisible(g_hTab)) ? 1 : 0,
             (g_hStatus && IsWindowVisible(g_hStatus)) ? 1 : 0);
        if (wcslen(buf) + wcslen(l) + 1 < 32768)
            wcsncat(buf, l, 32768 - wcslen(buf) - 1);
    }

    {
        size_t u8n = 0;
        char *u8 = wide_to_utf8(buf, &u8n);
        copy_text_to_clipboard(u8, u8n);
        free(u8);
    }
    MessageBoxW(g_hMain, buf, L"界面几何自检（内容已复制到剪贴板）",
                MB_OK | MB_ICONINFORMATION);
    free(buf);
    return 1;
}

/* ====================================================================
 * 端到端编码自检（命令行 /enc-test "<文件路径>"）
 *
 * 开发机无法运行 Windows 界面，所以这里把"文件 → Base64 → 文本框 → 回读解码"
 * 整条链路离线跑一遍并逐段回显。任何一段出问题都能在弹窗中直接指出：
 *   [1] 文件是否读全（读入字节数 vs 文件大小）
 *   [2] Base64 长度是否符合 4*ceil(n/3)
 *   [3] 能否写进文本框、文本框实际收到多少字符（不崩溃）
 *   [4] 从文本框回读 → 解码 → 与原始文件逐字节比对
 * ==================================================================== */

/* 把窄字符串的一段按 ASCII 复制成宽字符串（避免依赖 %S 的库间差异） */
static void ascii_slice(const char *s, size_t from, size_t n, wchar_t *out)
{
    size_t i;
    for (i = 0; i < n && s[from + i]; i++)
        out[i] = (wchar_t)(unsigned char)s[from + i];
    out[i] = L'\0';
}

/* 统计宽字符串中的非空白字符数（用于判断文本框内容是否被截断） */
static size_t count_non_space(const wchar_t *s)
{
    size_t n = 0;
    for (; s && *s; s++) {
        if (*s == L' ' || *s == L'\t' || *s == L'\r' || *s == L'\n')
            continue;
        n++;
    }
    return n;
}

static int handle_enc_test(void)
{
    const wchar_t *args = cmdline_args();
    const wchar_t *p;
    wchar_t path[PATHBUF];
    wchar_t *buf;
    unsigned char *raw = NULL, *back = NULL;
    unsigned long long rawLen = 0, diskSize = 0;
    size_t encLen = 0, got = 0;
    char *b64 = NULL;
    wchar_t err[512], line[1024], failMsg[512];
    size_t plen = 0;
    int failed = 0, failStep = 0;

    if (!args || !wcsstr(args, L"/enc-test"))
        return 0;

    /* ---- 解析路径：/enc-test 之后的部分，支持带引号、含空格 ---- */
    p = wcsstr(args, L"/enc-test") + 9;
    while (*p == L' ' || *p == L'\t')
        p++;
    path[0] = L'\0';
    if (*p == L'"') {
        p++;
        while (*p && *p != L'"' && plen + 1 < PATHBUF)
            path[plen++] = *p++;
    } else {
        while (*p && plen + 1 < PATHBUF)
            path[plen++] = *p++;
        while (plen && (path[plen - 1] == L' ' || path[plen - 1] == L'\t'))
            plen--;
    }
    path[plen] = L'\0';
    failMsg[0] = L'\0';

    buf = (wchar_t *)calloc(32768, sizeof(wchar_t));
    if (!buf)
        return 1;

    fmtw(line, 1024,
         L"[/enc-test 端到端编码自检]\r\n"
         L"目标文件 : %s\r\n"
         L"BASE64框 : %s      换行选项: %s\r\n\r\n",
         path[0] ? path : L"(未提供路径)",
         (g_txtEnc && IsWindow(g_txtEnc)) ? L"就绪" : L"不可用",
         (g_chkWrap && SendMessageW(g_chkWrap, BM_GETCHECK, 0, 0) == BST_CHECKED)
             ? L"开" : L"关");
    wcsncpy(buf, line, 32767);
    buf[32767] = L'\0';

    if (!path[0]) {
        wcsncat(buf, L"用法: B64ShaTool_x64.exe /enc-test \"C:\\路径\\图片.png\"\r\n",
                32767 - wcslen(buf));
        MessageBoxW(NULL, buf, L"端到端编码自检", MB_OK | MB_ICONWARNING);
        free(buf);
        return 1;
    }

    /* ---------- [1] 读取文件：这一步曾经就是崩溃点 ---------- */
    if (!read_whole_file(path, &raw, &rawLen, err, 512)) {
        failed = 1;
        failStep = 1;
        wcsncpy(failMsg, err, 511);
        failMsg[511] = L'\0';
        fmtw(line, 1024, L"[1] 读取文件 ......... 失败：%s\r\n", err);
        wcsncat(buf, line, 32767 - wcslen(buf));
    } else {
        HANDLE fh = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fh != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz;
            if (GetFileSizeEx(fh, &sz))
                diskSize = (unsigned long long)sz.QuadPart;
            CloseHandle(fh);
        }
        fmtw(line, 1024,
             L"[1] 读取文件 ......... 磁盘 %I64u 字节 / 读入 %I64u 字节  →  %s\r\n",
             diskSize, rawLen, (diskSize == rawLen) ? L"OK" : L"长度不一致（异常）");
        wcsncat(buf, line, 32767 - wcslen(buf));
        if (diskSize != rawLen) {
            failed = 1;
            failStep = 1;
            wcsncpy(failMsg, L"读入长度与文件大小不一致", 511);
            failMsg[511] = L'\0';
        }
    }

    /* ---------- [2] Base64 编码 ---------- */
    if (!failed) {
        wchar_t pre[64], suf[40];

        encLen = b64_encoded_len((size_t)rawLen);
        b64 = (char *)malloc(encLen + 1);
        if (!b64) {
            failed = 1;
            failStep = 2;
            wcsncpy(failMsg, L"内存不足，无法分配 Base64 缓冲", 511);
            failMsg[511] = L'\0';
        } else {
            b64_encode(raw, (size_t)rawLen, b64);
            got = strlen(b64);
            ascii_slice(b64, 0, 48, pre);
            ascii_slice(b64, (got > 24) ? got - 24 : 0, 24, suf);
            fmtw(line, 1024,
                 L"[2] Base64 编码 ...... 期望 %I64u 字符 / 实际 %I64u 字符  →  %s\r\n"
                 L"    开头: %s\r\n"
                 L"    结尾: %s\r\n",
                 (unsigned long long)encLen, (unsigned long long)got,
                 (got == encLen) ? L"OK" : L"长度不符（异常）", pre, suf);
            wcsncat(buf, line, 32767 - wcslen(buf));
            if (got != encLen) {
                failed = 1;
                failStep = 2;
                wcsncpy(failMsg, L"Base64 长度与期望不符", 511);
                failMsg[511] = L'\0';
            }
        }
    }

    /* ---------- [3][4] 写入文本框并回读校验（走真实渲染路径） ---------- */
    if (!failed && b64) {
        if (!g_txtEnc || !IsWindow(g_txtEnc)) {
            wcsncat(buf, L"[3] 写入文本框 ....... 跳过（界面控件不可用）\r\n",
                    32767 - wcslen(buf));
        } else {
            size_t boxLen, boxData;
            wchar_t *box;

            g_b64 = b64;               /* 交给真实的渲染函数，不用另写一套 */
            g_b64Len = encLen;
            render_b64_to_box();
            boxLen = (size_t)GetWindowTextLengthW(g_txtEnc);

            box = get_edit_text(g_txtEnc);
            boxData = count_non_space(box);

            fmtw(line, 1024,
                 L"[3] 写入文本框 ....... 文本框共 %I64u 字符（含换行），其中有效 Base64 字符 %I64u 个"
                 L"  →  %s\r\n",
                 (unsigned long long)boxLen, (unsigned long long)boxData,
                 (boxData == encLen || encLen > PREVIEW_LIMIT)
                     ? L"OK" : L"内容不完整（异常，可能被截断）");
            wcsncat(buf, line, 32767 - wcslen(buf));

            /* [4] 把文本框里的内容原样取回来解码，和原始文件逐字节比对 */
            if (box && boxLen > 0 && encLen <= PREVIEW_LIMIT) {
                size_t u8n = 0;
                char *u8 = wide_to_utf8(box, &u8n);
                size_t cap = b64_decoded_cap(u8n);
                size_t dn;

                back = (unsigned char *)malloc(cap ? cap : 1);
                dn = back ? b64_decode(u8, u8n, back, cap) : (size_t)-1;
                free(u8);

                if (dn == (size_t)-1) {
                    failed = 1;
                    failStep = 4;
                    wcsncpy(failMsg, L"从文本框回读的内容无法解码（可能被截断或含非法字符）", 511);
                    failMsg[511] = L'\0';
                    wcsncat(buf, L"[4] 回读解码一致性 ... 失败：内容无法解码\r\n",
                            32767 - wcslen(buf));
                } else {
                    int same = (dn == (size_t)rawLen) &&
                               (rawLen == 0 || memcmp(back, raw, (size_t)rawLen) == 0);
                    fmtw(line, 1024,
                         L"[4] 回读解码一致性 ... 解码得到 %I64u 字节 / 原文件 %I64u 字节  →  %s\r\n",
                         (unsigned long long)dn, rawLen,
                         same ? L"逐字节一致 OK" : L"不一致（异常）");
                    wcsncat(buf, line, 32767 - wcslen(buf));
                    if (!same) {
                        failed = 1;
                        failStep = 4;
                        wcsncpy(failMsg, L"回读解码结果与原文件不一致", 511);
                        failMsg[511] = L'\0';
                    }
                }
            } else if (encLen > PREVIEW_LIMIT) {
                wcsncat(buf, L"[4] 回读解码一致性 ... 跳过（结果超过文本框预览上限，属预期行为）\r\n",
                        32767 - wcslen(buf));
            } else {
                wcsncat(buf, L"[4] 回读解码一致性 ... 跳过（文本框为空）\r\n",
                        32767 - wcslen(buf));
            }
            free(box);

            g_b64 = NULL;              /* 不要把测试用的缓冲留成全局状态 */
            g_b64Len = 0;
        }
    }

    wcsncat(buf, L"\r\n结论: ", 32767 - wcslen(buf));
    if (failed) {
        fmtw(line, 1024, L"在第 [%d] 步失败 —— %s\r\n", failStep, failMsg);
        wcsncat(buf, line, 32767 - wcslen(buf));
    } else {
        wcsncat(buf, L"全部通过 —— 该文件的编码链路正常\r\n", 32767 - wcslen(buf));
    }

    {
        size_t u8n = 0;
        char *u8 = wide_to_utf8(buf, &u8n);
        copy_text_to_clipboard(u8, u8n);
        free(u8);
    }
    MessageBoxW(g_hMain, buf, L"端到端编码自检（内容已复制到剪贴板）",
                MB_OK | (failed ? MB_ICONERROR : MB_ICONINFORMATION));

    free(back);
    free(b64);
    free(raw);
    free(buf);
    return 1;
}

/* ====================================================================
 * 入口
 * ==================================================================== */

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmdLine, int nShow)
{
    INITCOMMONCONTROLSEX icc;
    WNDCLASSEXW wc;
    MSG msg;
    HDC hdc;
    int scrW, scrH, winW, winH;
    int uiOk = 0;

    (void)hPrev;
    (void)cmdLine;
    g_hInst = hInst;

    if (handle_selftest())
        return 0;

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES |
                ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    if (FAILED(OleInitialize(NULL)))
        return 1;                  /* SHBrowseForFolder 的“新建文件夹”按钮依赖 COM */

    hdc = GetDC(NULL);
    g_dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(NULL, hdc);
    if (g_dpi < 96)
        g_dpi = 96;

    g_fontUI = CreateFontW(-MulDiv(10, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"微软雅黑");
    g_fontMono = CreateFontW(-MulDiv(10, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    g_lineH = MulDiv(10, g_dpi, 72) + ds(12);
    g_brPanel = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));   /* app.rc 中 ID 为 1 的 RT_GROUP_ICON */
    wc.hIconSm = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = g_brPanel;
    wc.lpszClassName = L"B64ShaMain";
    if (!wc.hIcon) {
        wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
        wc.hIconSm = LoadIconW(NULL, IDI_APPLICATION);
    }
    if (!RegisterClassExW(&wc))
        return 1;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PanelProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = g_brPanel;
    wc.lpszClassName = L"B64Panel";
    if (!RegisterClassExW(&wc))
        return 1;

    scrW = GetSystemMetrics(SM_CXSCREEN);
    scrH = GetSystemMetrics(SM_CYSCREEN);
    winW = ds(1120);
    winH = ds(880);
    if (winW > scrW - ds(40))
        winW = scrW - ds(40);
    if (winH > scrH - ds(80))
        winH = scrH - ds(80);
    if (winW < ds(860)) winW = ds(860);
    if (winH < ds(660)) winH = ds(660);

    g_hMain = CreateWindowExW(WS_EX_ACCEPTFILES | WS_EX_CONTROLPARENT,
                              L"B64ShaMain", APP_TITLE,
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              (scrW - winW) / 2, (scrH - winH) / 3, winW, winH,
                              NULL, NULL, hInst, NULL);
    if (!g_hMain)
        return 1;

    /*
     * 兜底构建：正常情况下 WM_CREATE 里已经建好界面（靠 g_uiBuilt 去重，这里会直接返回）。
     * 万一某台机器上 WM_CREATE 的构建没能完成，这里还有第二次机会——此时 g_hMain
     * 已是有效句柄，构建条件必然成立。
     */
    build_ui(g_hMain);

    /*
     * 界面构件自检：宁可弹出一个说得清原因的错误框，也不要再留给用户一片空白窗口。
     * 一旦这里触发，弹窗里的类名与错误码足以直接定位问题。
     */
    {
        const wchar_t *args = cmdline_args();
        int wantDiag = (args && wcsstr(args, L"/diag")) ? 1 : 0;

        uiOk = (!g_hTab || !g_panel[0] || !g_panel[1] || !g_panel[2] ||
                !g_panel[3] || !g_panel[4] || !g_hStatus || !g_txtEnc ||
                !g_txtDec || !g_txtHashIn || !g_txtHashOut ||
                !apitest_ui_ok()) ? 0 : 1;

        /*
         * 第 5 页不参与致命判断：即使它的表格没建出来，前四页仍然可用，
         * 不该因为一个可选页就把整个工具拦在启动失败上。
         */
        if (uiOk && !batchtest_ui_ok())
            MessageBoxW(NULL,
                        L"第 5 页（批量接口测试）的界面没能完整创建，该页可能不可用。\n\n"
                        L"其余四个页签不受影响。",
                        L"提示", MB_OK | MB_ICONWARNING);

        if (!uiOk && !wantDiag) {
            wchar_t fatalMsg[1200];
            fmtw(fatalMsg, 1200,
                 L"界面控件创建失败，程序无法启动。\r\n\r\n"
                 L"主窗口: %p    构建时父窗口: %p\r\n"
                 L"创建失败 %d 次    最后错误码: %lu    最后失败的控件: %s\r\n\r\n"
                 L"tab=%p  status=%p  panel=%p/%p/%p/%p/%p\r\n"
                 L"编码框=%p  解码框=%p  摘要输入=%p  摘要输出=%p",
                 (void *)g_hMain, (void *)g_buildParent,
                 g_mkFails, (unsigned long)g_mkLastErr,
                 g_mkLastCls[0] ? g_mkLastCls : L"(无)",
                 (void *)g_hTab, (void *)g_hStatus,
                 (void *)g_panel[0], (void *)g_panel[1], (void *)g_panel[2],
                 (void *)g_panel[3], (void *)g_panel[4],
                 (void *)g_txtEnc, (void *)g_txtDec,
                 (void *)g_txtHashIn, (void *)g_txtHashOut);
            MessageBoxW(NULL, fatalMsg, L"启动失败", MB_OK | MB_ICONERROR);
            return 1;
        }
        /* 带 /diag 时不退出：继续走几何自检，把失败详情一并回显出来 */
    }

    if (uiOk)
        init_ui_state();

    /*
     * 某些启动方式（计划任务、RPA 宿主、部分外壳程序）会把 nShow 传成 SW_HIDE，
     * 那样窗口永远不会显示出来。这里统一兜底为正常显示，避免"启动了却看不见"。
     */
    if (nShow == SW_HIDE)
        nShow = SW_SHOWNORMAL;

    ShowWindow(g_hMain, nShow);
    UpdateWindow(g_hMain);

    /*
     * 关键一步：窗口创建时没有带 WS_VISIBLE，系统不会在创建阶段派发 WM_SIZE；
     * 之后 ShowWindow 只改变可见性、尺寸没变，也不会派发 WM_SIZE。
     * 所以如果只把布局挂在 WM_SIZE 上，layout() 就可能一次都不执行 ——
     * 界面会停留在"控件刚创建时的 10x10"状态，看上去就是整片空白。
     * 这里在窗口真正显示出来之后显式布局一次，保证首次呈现即正确。
     */
    layout();
    UpdateWindow(g_hMain);

    /* 连同子窗口一起重绘一遍：面板是刚显示/刚移动的，保证首帧内容立刻可见 */
    RedrawWindow(g_hMain, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    SetForegroundWindow(g_hMain);

    /* 端到端编码自检：整条编码链路跑一遍并回显，用于在目标机上定位问题 */
    if (handle_enc_test())
        return 0;

    /* 接口链路自检：读文件 -> Base64 -> 发请求 -> 解析 -> 存图 */
    if (handle_api_test())
        return 0;

    /* 界面几何自检：把真实布局回显出来，便于远程排查显示问题 */
    if (handle_diag())
        return 0;

    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(g_hMain, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    OleUninitialize();
    return (int)msg.wParam;
}
