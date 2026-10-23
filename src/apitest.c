/*
 * apitest.c - 第 4 个标签页「接口测试」
 *
 * 结构：
 *   1) api_run()：读文件 -> 识别类型 -> Base64 -> 拼 body -> 发请求
 *      -> 解析 errorCode/errorMsg -> 提取图片。不碰任何窗口句柄，
 *      因此既能在 worker 线程里跑，也能被 /api-test 命令行直接调用。
 *   2) 界面：固定请求区 + 可滚动参数区 + 响应区。
 *   3) 线程：界面线程派活，worker 干完 PostMessage 回来。
 *
 * 为什么必须用线程：WinHTTP 是同步阻塞的，OCR 请求动辄几十秒到几分钟。
 * 现有的「阻塞 + pump()」在 WinHttpReceiveResponse 内部阻塞时根本无法处理
 * 窗口消息，界面会直接进入「未响应」。
 */
#include "apitest.h"

#include <commctrl.h>

#include "apibody.h"
#include "b64sha.h"
#include "httpclient.h"
#include "jsonlite.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define A_PREVIEW_CAP    ((size_t)64 * 1024)
#define A_MAX_IMAGES     200
#define A_MAX_ONE_IMAGE  ((size_t)64 * 1024 * 1024)

/* 提示行唯一的文案：不再随所选文件的扩展名变化 */
#define A_HINT_TEXT \
    L"提示：点「发送请求」会把所选文件内容以 Base64 提交到上面的地址"

/* ==================================================================
 * 数据结构
 * ================================================================== */

typedef struct {
    wchar_t         path[1024];
    wchar_t         url[2048];
    int             method_get;
    int             file_type;          /* -1 = 自动，0/1 = 指定 */
    int             use_proxy;
    DWORD           timeout_ms;
    api_param_state params[API_MAX_PARAMS];
    HWND            notify;
    UINT_PTR        gen;
    int             dry;                /* 1 = 只跑到组装 body，不联网 */
} api_job;

struct api_result_tag {
    UINT_PTR     gen;
    int          net_ok;
    int          has_biz;
    long         biz_code;
    wchar_t      biz_msg[1024];

    hc_response  http;

    unsigned long long file_size;
    size_t       b64_len;
    size_t       body_len;
    char         body_sha[65];
    int          file_type;
    char         ext[16];
    int          ext_known;

    jl_image_list images;
    size_t       images_b64_chars;

    wchar_t      fail_stage[32];
    wchar_t      fail_msg[512];

    unsigned long long t_read_ms;
    unsigned long long t_b64_ms;
    unsigned long long t_total_ms;
};

typedef struct {
    HWND h;
    int  x, base_y, w, height;
} scroll_item;

/*
 * 参数区（30 项：功能开关 / 数值枚举 / 手写 JSON 片段）的控件与滚动状态。
 *
 * 做成一可实例化的结构，是因为第 4 页（单文件）与第 5 页（批量）各需要一套：
 * 两页参数互相独立，比"共用一套、切页时把父窗口搬来搬去"可靠得多
 * （搬父窗口要处理 z-order、焦点、重绘，实机上很容易出怪现象）。
 */
typedef struct {
    int   built;
    int   id_base;                  /* 600 = 单文件页，900 = 批量页 */
    int   vsb_id;
    HWND  host, vsb, hdr[3];
    HWND  chk[API_MAX_PARAMS];
    HWND  val[API_MAX_PARAMS];

    scroll_item items[96];
    int   item_count;
    int   content_h, view_h, scroll_pos, scroll_max;
} param_ui;

static struct {
    api_ui_ctx ctx;
    int        built;

    HWND lbl_url, url, method, send, clear;
    HWND lbl_file, file_edit, pick, ftype;
    HWND proxy, timeout, lbl_sec, hint;

    param_ui params;                /* 本页参数区 */

    HWND meta1, meta2, resp, copy_resp;
    HWND lbl_out, outdir, browse, save_img, save_txt, redact;

    wchar_t outdir_path[1024];
    int     meta2_bad;

    api_result *last;
    UINT_PTR    gen;
    LONG        busy;
} A;

/* 把纯 ASCII 的窄字符串（如扩展名）逐字节转成宽字符。
 * 不走 %S：宽格式串里 %s 与 %S 的宽窄语义在 msvcrt/UCRT 上容易搞反，
 * 一旦把 char* 当 wchar_t* 读，".png" 会变成两个乱码汉字，直接毁掉文件名。 */
static void ascii_to_wide(const char *s, wchar_t *out, size_t cch)
{
    size_t i = 0;

    if (!out || cch == 0)
        return;
    while (s && s[i] && i + 1 < cch) {
        out[i] = (wchar_t)(unsigned char)s[i];
        i++;
    }
    out[i] = L'\0';
}

/* ==================================================================
 * 小工具
 * ================================================================== */

/* 脱敏副本：预览 / 复制 / 保存 TXT 共用（实现在保存区） */
static int make_redacted(const api_result *r, char **out, size_t *outLen);

static void wset(wchar_t *dst, size_t cch, const wchar_t *fmt, ...)
{
    va_list ap;

    if (!dst || cch == 0)
        return;
    va_start(ap, fmt);
    _vsnwprintf(dst, cch - 1, fmt, ap);
    va_end(ap);
    dst[cch - 1] = L'\0';
}

static wchar_t *a_utf8_to_wide(const char *s, size_t len)
{
    int need;
    wchar_t *r;

    if (!s && len)
        return NULL;
    need = MultiByteToWideChar(CP_UTF8, 0, s, (int)len, NULL, 0);
    if (need <= 0) {
        r = (wchar_t *)malloc(sizeof(wchar_t));
        if (r)
            r[0] = L'\0';
        return r;
    }
    r = (wchar_t *)malloc(((size_t)need + 1) * sizeof(wchar_t));
    if (!r)
        return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, (int)len, r, need);
    r[need] = L'\0';
    return r;
}

static char *a_wide_to_utf8(const wchar_t *w, size_t *outLen)
{
    int need;
    char *r;

    if (outLen)
        *outLen = 0;
    if (!w)
        return NULL;
    need = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (need <= 0) {
        r = (char *)malloc(1);
        if (r)
            r[0] = '\0';
        return r;
    }
    r = (char *)malloc((size_t)need + 1);
    if (!r)
        return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, r, need, NULL, NULL);
    if (outLen)
        *outLen = (size_t)need - 1;
    return r;
}

static void copy_utf8_to_wide(const char *u8, wchar_t *out, size_t cch)
{
    wchar_t *w;

    if (!out || cch == 0)
        return;
    out[0] = L'\0';
    if (!u8)
        return;
    w = a_utf8_to_wide(u8, strlen(u8));
    if (!w)
        return;
    wset(out, cch, L"%s", w);
    free(w);
}

static wchar_t *get_text(HWND h)
{
    int len;
    wchar_t *buf;

    if (!h)
        return NULL;
    len = GetWindowTextLengthW(h);
    buf = (wchar_t *)malloc(((size_t)len + 2) * sizeof(wchar_t));
    if (!buf)
        return NULL;
    GetWindowTextW(h, buf, len + 1);
    buf[len] = L'\0';
    return buf;
}

static HWND main_window(void)
{
    return A.url ? GetAncestor(A.url, GA_ROOT) : NULL;
}

static void copy_clip(const char *utf8, size_t len)
{
    wchar_t *w = a_utf8_to_wide(utf8, len);
    HGLOBAL mem;
    size_t bytes;

    if (!w)
        return;
    bytes = (wcslen(w) + 1) * sizeof(wchar_t);
    if (!OpenClipboard(main_window())) {
        free(w);
        return;
    }
    EmptyClipboard();
    mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void *dst = GlobalLock(mem);
        if (dst) {
            memcpy(dst, w, bytes);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
    free(w);
}

static void msg_error(const wchar_t *fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;

    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    buf[2047] = L'\0';
    MessageBoxW(main_window(), buf, L"接口测试", MB_OK | MB_ICONWARNING);
}

static void msg_info(const wchar_t *fmt, ...)
{
    wchar_t buf[4096];
    va_list ap;

    va_start(ap, fmt);
    _vsnwprintf(buf, 4095, fmt, ap);
    va_end(ap);
    buf[4095] = L'\0';
    MessageBoxW(main_window(), buf, L"接口测试", MB_OK | MB_ICONINFORMATION);
}

static void size_text(unsigned long long n, wchar_t *out, size_t cch)
{
    if (n < 1024)
        wset(out, cch, L"%I64u 字节", n);
    else if (n < 1024ull * 1024)
        wset(out, cch, L"%.1f KB", (double)n / 1024.0);
    else
        wset(out, cch, L"%.2f MB", (double)n / (1024.0 * 1024.0));
}

/* ==================================================================
 * 业务链路（worker 与命令行共用）
 * ================================================================== */

static void result_free(api_result *r)
{
    if (!r)
        return;
    hc_response_free(&r->http);
    jl_image_list_free(&r->images);
}

static int api_run(const api_job *job, api_result *res)
{
    unsigned char *raw = NULL;
    unsigned long long rawLen = 0;
    char *b64 = NULL, *body = NULL;
    size_t encLen = 0, bodyLen = 0;
    wchar_t err[512];
    char    aerr[512];
    unsigned long long t0, t1;
    api_request_spec spec;
    hc_request hq;
    const char *ext;

    t0 = GetTickCount64();
    err[0] = L'\0';

    /* ---- 1. 读文件 ---- */
    if (!job->path[0]) {
        wset(res->fail_stage, 32, L"读取文件");
        wset(res->fail_msg, 512, L"还没有选择文件");
        return -1;
    }
    if (!A.ctx.read_file || !A.ctx.read_file(job->path, &raw, &rawLen, err, 512)) {
        wset(res->fail_stage, 32, L"读取文件");
        wset(res->fail_msg, 512, L"%s", err[0] ? err : L"读取失败");
        return -1;
    }
    res->file_size = rawLen;
    t1 = GetTickCount64();
    res->t_read_ms = t1 - t0;

    /* ---- 2. 识别类型 ---- */
    ext = detect_file_ext(raw, (size_t)rawLen, NULL);
    snprintf(res->ext, sizeof(res->ext), "%s", ext ? ext : ".bin");
    if (job->file_type < 0) {
        int known = 0;
        res->file_type = api_filetype_for_ext(res->ext, &known);
        res->ext_known = known;
    } else {
        res->file_type = job->file_type;
        res->ext_known = 1;
    }

    /* ---- 3. Base64 ---- */
    t0 = GetTickCount64();
    encLen = b64_encoded_len((size_t)rawLen);
    b64 = (char *)malloc(encLen + 1);
    if (!b64) {
        free(raw);
        wset(res->fail_stage, 32, L"Base64");
        wset(res->fail_msg, 512, L"内存不足，无法为 %I64u 字节的文件分配 Base64 缓冲",
             rawLen);
        return -1;
    }
    b64_encode(raw, (size_t)rawLen, b64);
    res->b64_len = encLen;
    free(raw);
    t1 = GetTickCount64();
    res->t_b64_ms = t1 - t0;

    /* ---- 4. 拼 body ---- */
    memset(&spec, 0, sizeof(spec));
    spec.method_get = job->method_get;
    spec.file_type = res->file_type;
    spec.file_b64 = b64;
    spec.file_b64_len = encLen;
    memcpy(spec.params, job->params, sizeof(spec.params));

    if (api_build_body(&spec, &body, &bodyLen, aerr, sizeof(aerr)) != 0) {
        free(b64);
        wset(res->fail_stage, 32, L"组装请求体");
        copy_utf8_to_wide(aerr, res->fail_msg, 512);
        return -1;
    }
    res->body_len = bodyLen;
    if (body && bodyLen)
        sha256_text(body, bodyLen, res->body_sha, 0);
    else
        snprintf(res->body_sha, sizeof(res->body_sha), "%s", "(GET 无请求体)");
    free(b64);

    if (job->dry) {                      /* /dry：只验到组装请求体，不联网 */
        free(body);
        res->net_ok = 1;
        res->t_total_ms = GetTickCount64() - t0;
        return 0;
    }

    /* ---- 5. 发请求 ---- */
    memset(&hq, 0, sizeof(hq));
    wset(hq.url, 2048, L"%s", job->url);
    hq.method_get = job->method_get;
    hq.body = body;
    hq.body_len = bodyLen;
    hq.use_system_proxy = job->use_proxy;
    hq.timeout_ms = job->timeout_ms;

    res->net_ok = (hc_execute(&hq, &res->http) == 0) ? 1 : 0;
    free(body);

    /* ---- 6. 解析 ---- */
    if (res->net_ok && res->http.data && res->http.len > 0) {
        static const char *keys[2];
        jl_summary sum;

        keys[0] = "errorCode";
        keys[1] = "errorMsg";
        if (jl_scan(res->http.data, res->http.len, keys, 2, &sum) == 0) {
            if (jl_field_int(&sum, "errorCode", &res->biz_code))
                res->has_biz = 1;
            {
                char msg8[1024];
                if (jl_field_str(&sum, "errorMsg", msg8, sizeof(msg8)))
                    copy_utf8_to_wide(msg8, res->biz_msg, 1024);
            }
            res->images_b64_chars = sum.images_b64_chars;
            res->images = sum.images;              /* 移交所有权 */
            memset(&sum.images, 0, sizeof(sum.images));
            jl_summary_free(&sum);
        }
    }

    res->t_total_ms = GetTickCount64() - (t0 - res->t_read_ms - res->t_b64_ms);

    if (!res->net_ok) {
        wset(res->fail_stage, 32, L"%s", res->http.stage);
        wset(res->fail_msg, 512, L"%s", res->http.err_text);
        return -1;
    }
    return 0;
}

/* ==================================================================
 * 界面构建
 * ================================================================== */

static void params_add_item(param_ui *p, HWND h, int x, int y, int w, int ht)
{
    if (!p || !h || p->item_count >= (int)(sizeof(p->items) / sizeof(p->items[0])))
        return;
    p->items[p->item_count].h = h;
    p->items[p->item_count].x = x;
    p->items[p->item_count].base_y = y;
    p->items[p->item_count].w = w;
    p->items[p->item_count].height = ht;
    p->item_count++;
}

/*
 * 创建一套参数控件。id_base 决定控件 ID 号段（600 / 900），
 * 两页各用一段，WM_COMMAND 才能分清是哪个页的哪个参数。
 */
static int params_build(param_ui *p, const api_ui_ctx *ctx, HWND panel,
                        int id_base, int vsb_id)
{
    static const wchar_t *HDR[3] = {
        L"功能开关（勾选后以 true 写入请求体）",
        L"数值 / 枚举参数（勾选后才参与请求）",
        L"手写 JSON 片段（勾选后原样拼入请求体）"
    };
    int i;

    if (!p || !ctx || !panel || !IsWindow(panel) || p->built)
        return -1;
    p->id_base = id_base;
    p->vsb_id = vsb_id;

    p->host = CreateWindowExW(WS_EX_CONTROLPARENT, L"B64Panel", L"",
                              WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                              0, 0, 100, 100, panel, NULL,
                              ctx->inst, NULL);
    p->vsb = CreateWindowExW(0, L"SCROLLBAR", L"",
                             WS_CHILD | WS_VISIBLE | SBS_VERT,
                             0, 0, 16, 100, panel, (HMENU)(INT_PTR)vsb_id,
                             ctx->inst, NULL);

    for (i = 0; i < 3; i++)
        p->hdr[i] = ctx->mk(p->host, L"STATIC", HDR[i], SS_LEFT, 0, 0,
                             ctx->font_ui);

    for (i = 0; i < (int)api_param_count(); i++) {
        const api_param_def *d = api_param_at((size_t)i);
        wchar_t label[256];

        wset(label, 256, L"%S", d->name);
        /*
         * 每个复选框/值控件必须有自己的 ID：勾选产生的 WM_COMMAND 靠 ID
         * 定位到具体参数，ID 为 0 时值框会一直停在禁用状态。
         */
        p->chk[i] = ctx->mk(p->host, L"BUTTON", label,
                             BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                             id_base + i * 2, ctx->font_ui);

        if (d->type == AP_NUM || d->type == AP_RAW) {
            p->val[i] = ctx->mk(p->host, L"EDIT", L"",
                                 ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE,
                                 id_base + i * 2 + 1, ctx->font_mono);
            if (p->val[i])
                SendMessageW(p->val[i], EM_SETLIMITTEXT,
                             (WPARAM)(API_VALUE_MAX - 1), 0);
        } else if (d->type == AP_ENUM) {
            const char *q = d->opts;

            p->val[i] = ctx->mk(p->host, L"COMBOBOX", L"",
                                 CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0,
                                 id_base + i * 2 + 1, ctx->font_ui);
            while (p->val[i] && q && *q) {
                const char *bar = strchr(q, '|');
                size_t len = bar ? (size_t)(bar - q) : strlen(q);
                wchar_t w[64];
                size_t k;

                for (k = 0; k < len && k + 1 < 64; k++)
                    w[k] = (wchar_t)(unsigned char)q[k];
                w[k] = L'\0';
                SendMessageW(p->val[i], CB_ADDSTRING, 0, (LPARAM)w);
                if (!bar)
                    break;
                q = bar + 1;
            }
        }
        if (p->val[i])
            EnableWindow(p->val[i], FALSE);   /* 未勾选时不可编辑，勾选后才放开 */
    }

    p->built = 1;
    return 0;
}

/*
 * 按文件名后缀粗略判断 fileType（最终以文件内容为准，在 api_run 里做）。
 * 只负责把下拉框切到对应项，不写任何说明文字 —— 提示行固定留给
 * "发送会把文件内容提交到上面地址" 那句合规提示。
 */
static void guess_type_from_name(const wchar_t *path)
{
    const wchar_t *dot = wcsrchr(path, L'.');
    char ext[24];
    char lower[24];
    size_t i, n = 0;
    int known = 0, t;

    if (!dot || !dot[1]) {
        SendMessageW(A.ftype, CB_SETCURSEL, 1, 0);
        return;
    }
    for (i = 1; dot[i] && n + 2 < sizeof(ext); i++)
        ext[n++] = (char)(dot[i] < 128 ? dot[i] : '?');
    ext[n] = '\0';

    lower[0] = '.';
    for (i = 0; ext[i] && i + 2 < sizeof(lower); i++) {
        char c = ext[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        lower[i + 1] = c;
    }
    lower[i + 1] = '\0';

    t = api_filetype_for_ext(lower, &known);
    SendMessageW(A.ftype, CB_SETCURSEL, t ? 1 : 0, 0);
    (void)known;
}

static void set_source(const wchar_t *path)
{
    SetWindowTextW(A.file_edit, path);
    guess_type_from_name(path);
}

int apitest_build(HWND panel, const api_ui_ctx *ctx)
{
    int i;
    static const wchar_t *METHODS[] = { L"POST", L"GET" };
    static const wchar_t *FTYPES[] = { L"0 - PDF", L"1 - 图片 / TIFF" };

    if (!panel || !IsWindow(panel) || !ctx)
        return -1;
    if (A.built)
        return 0;
    A.ctx = *ctx;
    A.built = 1;

    A.lbl_url = ctx->mk(panel, L"STATIC", L"接口地址:", SS_LEFT, 0, 0, ctx->font_ui);
    A.url = ctx->mk(panel, L"EDIT", L"http://127.0.0.1:8080/layout-parsing",
                    ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, IDA_URL, ctx->font_ui);
    A.method = ctx->mk(panel, L"COMBOBOX", L"",
                       CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, IDA_METHOD, ctx->font_ui);
    for (i = 0; i < 2; i++)
        SendMessageW(A.method, CB_ADDSTRING, 0, (LPARAM)METHODS[i]);
    SendMessageW(A.method, CB_SETCURSEL, 0, 0);

    A.send = ctx->mk(panel, L"BUTTON", L"发送请求", BS_PUSHBUTTON | WS_TABSTOP, 0,
                     IDA_SEND, ctx->font_ui);
    A.clear = ctx->mk(panel, L"BUTTON", L"清空", BS_PUSHBUTTON | WS_TABSTOP, 0,
                      IDA_CLEAR, ctx->font_ui);

    A.lbl_file = ctx->mk(panel, L"STATIC", L"源文件:", SS_LEFT, 0, 0, ctx->font_ui);
    A.file_edit = ctx->mk(panel, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP,
                          WS_EX_CLIENTEDGE, IDA_FILE_EDIT, ctx->font_ui);
    A.pick = ctx->mk(panel, L"BUTTON", L"选择文件…", BS_PUSHBUTTON | WS_TABSTOP, 0,
                     IDA_PICK_FILE, ctx->font_ui);
    A.ftype = ctx->mk(panel, L"COMBOBOX", L"",
                      CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, IDA_FILETYPE, ctx->font_ui);
    for (i = 0; i < 2; i++)
        SendMessageW(A.ftype, CB_ADDSTRING, 0, (LPARAM)FTYPES[i]);
    SendMessageW(A.ftype, CB_SETCURSEL, 1, 0);

    A.proxy = ctx->mk(panel, L"BUTTON", L"使用系统代理", BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                      IDA_PROXY, ctx->font_ui);
    A.timeout = ctx->mk(panel, L"EDIT", L"10", ES_AUTOHSCROLL | ES_NUMBER | WS_TABSTOP,
                        WS_EX_CLIENTEDGE, IDA_TIMEOUT, ctx->font_ui);
    A.lbl_sec = ctx->mk(panel, L"STATIC", L"秒超时", SS_LEFT, 0, 0, ctx->font_ui);
    A.hint = ctx->mk(panel, L"STATIC", L"", SS_LEFTNOWORDWRAP, 0, IDA_HINT, ctx->font_ui);

    /* 参数区（30 项）—— 第 4 页用 600 号段 */
    if (params_build(&A.params, ctx, panel, IDA_PARAM_BASE, IDA_VSCROLL) != 0)
        return -1;

    A.meta1 = ctx->mk(panel, L"STATIC", L"尚未发送请求。", SS_LEFTNOWORDWRAP, 0,
                      IDA_META1, ctx->font_ui);
    A.meta2 = ctx->mk(panel, L"STATIC", L"", SS_LEFTNOWORDWRAP, 0, IDA_META2, ctx->font_ui);
    A.resp = ctx->mk(panel, L"EDIT", L"",
                     ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | WS_TABSTOP,
                     WS_EX_CLIENTEDGE, IDA_RESP, ctx->font_mono);
    if (A.resp)
        SendMessageW(A.resp, EM_SETLIMITTEXT, (WPARAM)0x7FFFFFF0u, 0);

    A.copy_resp = ctx->mk(panel, L"BUTTON", L"复制响应", BS_PUSHBUTTON | WS_TABSTOP, 0,
                          IDA_COPY_RESP, ctx->font_ui);
    A.lbl_out = ctx->mk(panel, L"STATIC", L"保存到:", SS_LEFT, 0, 0, ctx->font_ui);
    A.outdir = ctx->mk(panel, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP,
                       WS_EX_CLIENTEDGE, IDA_OUTDIR, ctx->font_ui);
    A.browse = ctx->mk(panel, L"BUTTON", L"浏览…", BS_PUSHBUTTON | WS_TABSTOP, 0,
                       IDA_BROWSE, ctx->font_ui);
    A.save_img = ctx->mk(panel, L"BUTTON", L"保存图片", BS_PUSHBUTTON | WS_TABSTOP, 0,
                         IDA_SAVE_IMG, ctx->font_ui);
    A.save_txt = ctx->mk(panel, L"BUTTON", L"保存为 TXT…", BS_PUSHBUTTON | WS_TABSTOP, 0,
                         IDA_SAVE_TXT, ctx->font_ui);
    /*
     * 「替换 base64」：勾选后，"保存为 TXT…" 与 "复制响应" 都会把图片的
     * Base64 换成 "..."；不勾选则原样保留完整 Base64。
     * 默认不勾 —— 保留完整响应更接近"所见即所得"，需要小体积时再手动勾。
     */
    A.redact = ctx->mk(panel, L"BUTTON", L"替换 base64",
                       BS_AUTOCHECKBOX | WS_TABSTOP, 0, IDA_REDACT, ctx->font_ui);
    if (A.redact)
        SendMessageW(A.redact, BM_SETCHECK, BST_UNCHECKED, 0);

    return 0;
}

/*
 * 把参数控件恢复到默认值（勾选状态 + 值 + 可用状态）。
 * 两页各调用一次，互不影响。
 */
static void params_defaults(param_ui *p)
{
    api_param_state defs[API_MAX_PARAMS];
    size_t i;

    if (!p || !p->built)
        return;
    api_params_default(defs);

    for (i = 0; i < api_param_count(); i++) {
        const api_param_def *d = api_param_at(i);

        if (p->chk[i])
            SendMessageW(p->chk[i], BM_SETCHECK,
                         defs[i].on ? BST_CHECKED : BST_UNCHECKED, 0);
        if (!p->val[i])
            continue;

        if (d->type == AP_ENUM) {
            int k, cnt = (int)SendMessageW(p->val[i], CB_GETCOUNT, 0, 0);
            const char *q = d->opts;

            for (k = 0; k < cnt && q && *q; k++) {
                const char *bar = strchr(q, '|');
                size_t len = bar ? (size_t)(bar - q) : strlen(q);

                if (strlen(defs[i].value) == len &&
                    strncmp(q, defs[i].value, len) == 0) {
                    SendMessageW(p->val[i], CB_SETCURSEL, (WPARAM)k, 0);
                    break;
                }
                if (!bar)
                    break;
                q = bar + 1;
            }
        } else {
            wchar_t w[API_VALUE_MAX];
            wset(w, API_VALUE_MAX, L"%S", defs[i].value);
            SetWindowTextW(p->val[i], w);
        }
        EnableWindow(p->val[i], defs[i].on ? TRUE : FALSE);
    }
}

void apitest_init_state(void)
{
    if (!A.built)
        return;

    params_defaults(&A.params);

    if (A.outdir_path[0])
        SetWindowTextW(A.outdir, A.outdir_path);
    if (A.redact)
        SendMessageW(A.redact, BM_SETCHECK, BST_UNCHECKED, 0);
    if (A.hint)
        SetWindowTextW(A.hint, A_HINT_TEXT);
}

/* ==================================================================
 * 布局与滚动
 * ================================================================== */

static void params_layout_content(param_ui *p, int hostW)
{
    int gap = A.ctx.ds(8);
    int rh = A.ctx.line_h();
    int y = A.ctx.ds(2);
    int i, n = (int)api_param_count();
    int bools = 0, vals = 0, raws = 0;
    int cell, rows;

    p->item_count = 0;

    for (i = 0; i < n; i++) {
        if (api_param_at((size_t)i)->group == AP_GROUP_SWITCH) bools++;
        else if (api_param_at((size_t)i)->group == AP_GROUP_VALUE) vals++;
        else raws++;
    }

    /* ---- 功能开关：3 列 ---- */
    params_add_item(p, p->hdr[0], 0, y, hostW, rh);
    y += rh + 2;
    cell = (hostW - gap * 2) / 3;
    if (cell < A.ctx.ds(150))
        cell = A.ctx.ds(150);
    rows = (bools + 2) / 3;
    {
        int k = 0;
        for (i = 0; i < n; i++) {
            if (api_param_at((size_t)i)->group != AP_GROUP_SWITCH)
                continue;
            params_add_item(p, p->chk[i], (k % 3) * (cell + gap), y + (k / 3) * (rh - 2),
                     cell, rh - 2);
            k++;
        }
    }
    y += rows * (rh - 2) + gap + 2;

    /* ---- 数值 / 枚举：2 列，每格 = 名称 + 值 ---- */
    params_add_item(p, p->hdr[1], 0, y, hostW, rh);
    y += rh + 2;
    {
        int k = 0;
        int cw = (hostW - gap) / 2;
        int nameW = A.ctx.ds(200);
        int editW = cw - nameW - gap;

        if (editW < A.ctx.ds(60))
            editW = A.ctx.ds(60);
        for (i = 0; i < n; i++) {
            const api_param_def *d = api_param_at((size_t)i);
            int x, yy, vh;

            if (d->group != AP_GROUP_VALUE)
                continue;
            x = (k % 2) * (cw + gap);
            yy = y + (k / 2) * (rh + 6);
            vh = (d->type == AP_ENUM) ? (rh + 160) : rh;
            params_add_item(p, p->chk[i], x, yy + 2, nameW, rh - 2);
            params_add_item(p, p->val[i], x + nameW + 2, yy, editW, vh);
            k++;
        }
        rows = (vals + 1) / 2;
    }
    y += rows * (rh + 6) + gap + 2;

    /* ---- 手写片段：整行 ---- */
    params_add_item(p, p->hdr[2], 0, y, hostW, rh);
    y += rh + 2;
    {
        int nameW = A.ctx.ds(220);
        int k = 0;

        for (i = 0; i < n; i++) {
            const api_param_def *d = api_param_at((size_t)i);
            int yy;

            if (d->group != AP_GROUP_RAW)
                continue;
            yy = y + k * (rh + 6);
            params_add_item(p, p->chk[i], 0, yy + 2, nameW, rh - 2);
            params_add_item(p, p->val[i], nameW + 2, yy, hostW - nameW - 2, rh);
            k++;
        }
        y += raws * (rh + 6);
    }

    p->content_h = y + A.ctx.ds(4);
}

static void params_apply_scroll(param_ui *p)
{
    HDWP dwp;
    int i;

    if (p->item_count == 0)
        return;
    dwp = BeginDeferWindowPos(p->item_count);
    if (!dwp)
        return;
    for (i = 0; i < p->item_count; i++) {
        dwp = DeferWindowPos(dwp, p->items[i].h, NULL,
                             p->items[i].x, p->items[i].base_y - p->scroll_pos,
                             p->items[i].w, p->items[i].height,
                             SWP_NOZORDER | SWP_NOACTIVATE);
        if (!dwp)
            return;
    }
    EndDeferWindowPos(dwp);
}

static void params_update_scrollbar(param_ui *p)
{
    SCROLLINFO si;

    p->scroll_max = (p->content_h > p->view_h) ? (p->content_h - p->view_h) : 0;
    if (p->scroll_pos > p->scroll_max)
        p->scroll_pos = p->scroll_max;
    if (p->scroll_pos < 0)
        p->scroll_pos = 0;

    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = (p->content_h > 0) ? (p->content_h - 1) : 0;
    si.nPage = (UINT)((p->view_h > 0) ? p->view_h : 1);
    si.nPos = p->scroll_pos;
    if (p->vsb)
        SetScrollInfo(p->vsb, SB_CTL, &si, TRUE);
    if (p->vsb)
        ShowWindow(p->vsb, (p->scroll_max > 0) ? SW_SHOW : SW_HIDE);
}

void apitest_layout(int px, int py, int pw, int ph)
{
    int m, gap, rh, x0, iw, y;
    int lblW, mtdW, sndW, clrW, pickW, ftW, rhCombo;
    int hostTop, hostBottom, hostH, hostW, respH, respTop, y1, y0;

    if (!A.built || !A.url)
        return;

    m = A.ctx.ds(12);
    gap = A.ctx.ds(8);
    rh = A.ctx.line_h();
    x0 = px + m;
    iw = pw - 2 * m;
    y = py + m;

    lblW = A.ctx.ds(64);
    mtdW = A.ctx.ds(90);
    sndW = A.ctx.ds(110);
    clrW = A.ctx.ds(64);
    pickW = A.ctx.ds(96);
    ftW = A.ctx.ds(96);
    rhCombo = rh + 160;

    /* 行 1：接口地址 + 方法 + 发送 + 清空 */
    MoveWindow(A.lbl_url, x0, y + A.ctx.ds(3), lblW, rh, TRUE);
    MoveWindow(A.url, x0 + lblW, y,
               iw - lblW - mtdW - sndW - clrW - gap * 3, rh, TRUE);
    MoveWindow(A.method, x0 + iw - mtdW - sndW - clrW - gap * 2, y, mtdW, rhCombo, TRUE);
    MoveWindow(A.send, x0 + iw - sndW - clrW - gap, y, sndW, rh, TRUE);
    MoveWindow(A.clear, x0 + iw - clrW, y, clrW, rh, TRUE);

    /* 行 2：源文件 + 选择 + fileType */
    y += rh + gap;
    MoveWindow(A.lbl_file, x0, y + A.ctx.ds(3), lblW, rh, TRUE);
    MoveWindow(A.file_edit, x0 + lblW, y,
               iw - lblW - pickW - ftW - gap * 2, rh, TRUE);
    MoveWindow(A.pick, x0 + iw - pickW - ftW - gap, y, pickW, rh, TRUE);
    MoveWindow(A.ftype, x0 + iw - ftW, y, ftW, rhCombo, TRUE);

    /* 行 3：代理 + 超时 + 提示 */
    y += rh + gap;
    MoveWindow(A.proxy, x0, y + A.ctx.ds(2), A.ctx.ds(120), rh - 2, TRUE);
    MoveWindow(A.timeout, x0 + A.ctx.ds(126), y, A.ctx.ds(60), rh, TRUE);
    MoveWindow(A.lbl_sec, x0 + A.ctx.ds(190), y + A.ctx.ds(3), A.ctx.ds(56), rh, TRUE);
    MoveWindow(A.hint, x0 + A.ctx.ds(250), y + A.ctx.ds(3),
               iw - A.ctx.ds(250), rh, TRUE);

    /* ---- 响应区（贴底） ---- */
    respH = ph * 40 / 100;
    if (respH < A.ctx.ds(190))
        respH = A.ctx.ds(190);
    respTop = py + ph - m - respH;

    hostTop = y + rh + gap;
    hostBottom = respTop - gap;
    hostH = hostBottom - hostTop;
    if (hostH < A.ctx.ds(80))
        hostH = A.ctx.ds(80);
    hostW = iw - A.ctx.ds(20);

    MoveWindow(A.params.vsb, x0 + iw - A.ctx.ds(16), hostTop, A.ctx.ds(16), hostH, TRUE);
    MoveWindow(A.params.host, x0, hostTop, hostW, hostH, TRUE);

    A.params.view_h = hostH;
    params_layout_content(&A.params, hostW);
    params_update_scrollbar(&A.params);
    params_apply_scroll(&A.params);

    /* ---- 响应区内部 ---- */
    MoveWindow(A.meta1, x0, respTop, iw, rh, TRUE);
    MoveWindow(A.meta2, x0, respTop + rh, iw, rh, TRUE);

    y0 = py + ph - m - rh;                 /* 按钮行 */
    y1 = y0 - gap - rh;                    /* 保存目录行 */

    MoveWindow(A.resp, x0, respTop + rh * 2 + A.ctx.ds(4), iw,
               y1 - gap - (respTop + rh * 2 + A.ctx.ds(4)), TRUE);

    MoveWindow(A.lbl_out, x0, y1 + A.ctx.ds(3), A.ctx.ds(56), rh, TRUE);
    MoveWindow(A.outdir, x0 + A.ctx.ds(56), y1,
               iw - A.ctx.ds(56) - A.ctx.ds(80) - gap, rh, TRUE);
    MoveWindow(A.browse, x0 + iw - A.ctx.ds(80), y1, A.ctx.ds(80), rh, TRUE);

    {
        int x = x0;
        int w1 = A.ctx.ds(96), w2 = A.ctx.ds(96), w3 = A.ctx.ds(120);
        int w4 = A.ctx.ds(110);

        MoveWindow(A.copy_resp, x, y0, w1, rh, TRUE); x += w1 + gap;
        MoveWindow(A.save_img, x, y0, w2, rh, TRUE);  x += w2 + gap;
        MoveWindow(A.save_txt, x, y0, w3, rh, TRUE);  x += w3 + gap * 2;
        MoveWindow(A.redact, x, y0 + A.ctx.ds(2), w4, rh - 2, TRUE);
    }
}

int apitest_ui_ok(void)
{
    return (A.built && A.url && A.send && A.params.host && A.params.vsb && A.resp &&
            A.meta1 && A.pick) ? 1 : 0;
}

void apitest_focus_target(void)
{
    if (A.url)
        SetFocus(A.url);
}

/* ==================================================================
 * 参数收集
 * ================================================================== */

static void put_wide_as_utf8(const wchar_t *w, char *out, size_t cap)
{
    char *u;
    size_t n = 0;

    if (cap == 0)
        return;
    out[0] = '\0';
    if (!w)
        return;
    u = a_wide_to_utf8(w, &n);
    if (!u)
        return;
    if (n >= cap)
        n = cap - 1;
    memcpy(out, u, n);
    out[n] = '\0';
    free(u);
}

static void params_collect(param_ui *p, api_param_state *ps)
{
    size_t i, n = api_param_count();

    memset(ps, 0, sizeof(api_param_state) * API_MAX_PARAMS);
    for (i = 0; i < n; i++) {
        const api_param_def *d = api_param_at(i);

        ps[i].on = (SendMessageW(p->chk[i], BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
        if (!p->val[i] || d->type == AP_BOOL)
            continue;

        if (d->type == AP_ENUM) {
            int sel = (int)SendMessageW(p->val[i], CB_GETCURSEL, 0, 0);
            wchar_t w[API_VALUE_MAX];

            w[0] = L'\0';
            if (sel >= 0 &&
                SendMessageW(p->val[i], CB_GETLBTEXT, (WPARAM)sel, (LPARAM)w) >= 0)
                put_wide_as_utf8(w, ps[i].value, API_VALUE_MAX);
        } else {
            wchar_t *w = get_text(p->val[i]);
            put_wide_as_utf8(w, ps[i].value, API_VALUE_MAX);
            free(w);
        }
    }
}

/* ==================================================================
 * 结果渲染
 * ================================================================== */

static void render_result(void)
{
    api_result *r = A.last;
    wchar_t line[2048], sz[64], imgsz[64];

    if (!r)
        return;

    if (!r->net_ok) {
        A.meta2_bad = 1;
        wset(line, 2048, L"网络失败（阶段：%s，错误码 %lu）",
             r->http.stage[0] ? r->http.stage : L"未知",
             (unsigned long)r->http.win32_err);
        SetWindowTextW(A.meta1, line);
        SetWindowTextW(A.meta2, r->http.err_text);
        SetWindowTextW(A.resp, L"");
        InvalidateRect(A.meta1, NULL, TRUE);
        InvalidateRect(A.meta2, NULL, TRUE);
        return;
    }

    size_text((unsigned long long)r->http.len, sz, sizeof(sz) / sizeof(sz[0]));
    wset(line, 2048, L"HTTP %d %s　·　耗时 %I64u ms　·　响应 %s",
         r->http.http_status,
         r->http.status_text[0] ? r->http.status_text : L"",
         r->http.elapsed_ms, sz);
    SetWindowTextW(A.meta1, line);

    if (r->has_biz && r->biz_code != 0) {
        A.meta2_bad = 1;
        wset(line, 2048, L"业务错误 errorCode=%ld：%s",
             r->biz_code, r->biz_msg[0] ? r->biz_msg : L"（响应里没有 errorMsg）");
    } else if (r->http.http_status >= 400) {
        A.meta2_bad = 1;
        wset(line, 2048, L"HTTP %d：服务端没有返回成功响应，请看下方响应内容",
             r->http.http_status);
    } else {
        A.meta2_bad = 0;
        size_text(r->http.len, sz, sizeof(sz) / sizeof(sz[0]));
        if (r->images.count > 0) {
            size_text((unsigned long long)r->images_b64_chars, imgsz,
                      sizeof(imgsz) / sizeof(imgsz[0]));
            wset(line, 2048,
                 L"图片 %Iu 张（Base64 约 %s）　·　请求体 %s　·　fileType=%d（%S）",
                 r->images.count, imgsz,
                 r->body_len ? L"已生成" : L"无", r->file_type, r->ext);
        } else {
            wset(line, 2048, L"未从响应中识别到图片　·　fileType=%d（%S）",
                 r->file_type, r->ext);
        }
    }
    SetWindowTextW(A.meta2, line);
    InvalidateRect(A.meta1, NULL, TRUE);
    InvalidateRect(A.meta2, NULL, TRUE);

    /* 预览：先脱敏（把图片 Base64 换成 "..."），再截断 */
    {
        char *red = NULL;
        size_t redLen = 0;
        char *prev = NULL;
        size_t prevLen = 0;
        int truncated = 0;
        wchar_t *w = NULL;

        if (!r->http.data) {
            SetWindowTextW(A.resp, L"");
            return;
        }

        if (make_redacted(r, &red, &redLen) != 0) {
            SetWindowTextW(A.resp, L"【内存不足，无法生成预览】");
            return;
        }
        if (jl_truncate_preview(red, redLen, A_PREVIEW_CAP, &prev, &prevLen,
                                &truncated) != 0) {
            free(red);
            SetWindowTextW(A.resp, L"【内存不足，无法生成预览】");
            return;
        }
        free(red);

        if (!jl_is_json(prev, prevLen)) {
            /* 不是 JSON（网关错误页、纯文本等）：照样显示，但明确告知 */
            wchar_t *t = a_utf8_to_wide(prev, prevLen > 4000 ? 4000 : prevLen);
            if (t) {
                wchar_t *full = (wchar_t *)malloc((wcslen(t) + 200) * sizeof(wchar_t));
                if (full) {
                    wset(full, wcslen(t) + 200,
                         L"【响应不是 JSON，以下是前 4000 字节原文】\r\n\r\n%s", t);
                    SetWindowTextW(A.resp, full);
                    free(full);
                }
                free(t);
            }
            free(prev);
            return;
        }

        w = a_utf8_to_wide(prev, prevLen);
        if (w) {
            if (truncated) {
                wchar_t *full = (wchar_t *)malloc(
                    (wcslen(w) + 200) * sizeof(wchar_t));
                if (full) {
                    wset(full, wcslen(w) + 200,
                         L"%s\r\n\r\n…（预览已截断，完整内容共 %I64u 字节，"
                         L"请用「保存为 TXT…」获取）", w, r->http.len);
                    SetWindowTextW(A.resp, full);
                    free(full);
                } else {
                    SetWindowTextW(A.resp, w);
                }
            } else {
                SetWindowTextW(A.resp, w);
            }
            free(w);
        }
        free(prev);
    }
}

/* ==================================================================
 * 忙碌状态
 * ================================================================== */

/* 参数控件整体启用/禁用；值控件还要跟随各自复选框的勾选状态 */
static void params_set_enabled(param_ui *p, int enabled)
{
    size_t i;

    if (!p)
        return;
    for (i = 0; i < api_param_count(); i++) {
        if (p->chk[i])
            EnableWindow(p->chk[i], enabled ? TRUE : FALSE);
        if (p->val[i])
            EnableWindow(p->val[i],
                         (enabled && SendMessageW(p->chk[i], BM_GETCHECK, 0, 0) == BST_CHECKED)
                             ? TRUE : FALSE);
    }
}

static void set_busy_ui(int busy)
{
    EnableWindow(A.send, !busy);
    EnableWindow(A.pick, !busy);
    EnableWindow(A.url, !busy);
    EnableWindow(A.method, !busy);
    EnableWindow(A.ftype, !busy);
    EnableWindow(A.proxy, !busy);
    EnableWindow(A.timeout, !busy);
    EnableWindow(A.clear, !busy);
    params_set_enabled(&A.params, !busy);
}

/* ==================================================================
 * 发送
 * ================================================================== */

static DWORD WINAPI worker_main(LPVOID param)
{
    api_job *job = (api_job *)param;
    api_result *res = (api_result *)calloc(1, sizeof(*res));

    if (!res) {
        free(job);
        return 0;
    }
    res->gen = job->gen;
    api_run(job, res);

    /*
     * 释放规则只有两条：
     *   PostMessage 成功 -> 结果归界面线程所有，worker 不再触碰；
     *   PostMessage 失败（窗口已销毁）-> worker 自己释放，避免泄漏。
     */
    if (!PostMessageW(job->notify, WM_API_DONE, (WPARAM)job->gen, (LPARAM)res)) {
        result_free(res);
        free(res);
    }
    free(job);
    return 0;
}

static void do_send(void)
{
    api_job *job;
    HANDLE th;
    wchar_t *url = NULL, *fp = NULL, *tmo = NULL;
    int tsec;
    DWORD tid = 0;

    if (InterlockedCompareExchange(&A.busy, 1, 0) != 0)
        return;

    url = get_text(A.url);
    fp = get_text(A.file_edit);

    if (!url || !url[0]) {
        msg_error(L"请先填写接口地址，例如：\nhttp://10.1.2.3:8080/layout-parsing");
        goto fail_free;
    }
    if (!fp || !fp[0]) {
        msg_error(L"请先点「选择文件…」选一个要上传的 PDF 或图片。");
        goto fail_free;
    }

    tmo = get_text(A.timeout);
    tsec = tmo ? (int)wcstol(tmo, NULL, 10) : 180;
    if (tsec < 5)
        tsec = 5;
    if (tsec > 3600)
        tsec = 3600;

    job = (api_job *)calloc(1, sizeof(*job));
    if (!job) {
        msg_error(L"内存不足，无法发起请求。");
        goto fail_free;
    }

    wset(job->path, 1024, L"%s", fp);
    wset(job->url, 2048, L"%s", url);
    job->method_get = (SendMessageW(A.method, CB_GETCURSEL, 0, 0) == 1) ? 1 : 0;
    job->file_type = (SendMessageW(A.ftype, CB_GETCURSEL, 0, 0) == 0) ? 0 : 1;
    job->use_proxy = (SendMessageW(A.proxy, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    job->timeout_ms = (DWORD)tsec * 1000u;
    params_collect(&A.params, job->params);
    job->notify = main_window();

    /* 上一轮结果不再需要，先释放，避免多占一份几十 MB 的响应 */
    if (A.last) {
        result_free(A.last);
        free(A.last);
        A.last = NULL;
    }

    A.gen++;
    job->gen = A.gen;

    {
        wchar_t line[512];
        wset(line, 512, L"请求中…（POST/GET 详见上方，超时 %d 秒；界面可正常操作）", tsec);
        SetWindowTextW(A.meta1, line);
        SetWindowTextW(A.meta2, L"");
        SetWindowTextW(A.resp, L"");
        A.meta2_bad = 0;
        InvalidateRect(A.meta2, NULL, TRUE);
    }

    th = CreateThread(NULL, 0, worker_main, job, 0, &tid);
    if (!th) {
        free(job);
        msg_error(L"无法创建工作线程（错误码 %lu）。", (unsigned long)GetLastError());
        goto fail_free;
    }
    CloseHandle(th);                 /* 分离：结果通过消息回传 */
    set_busy_ui(1);

    free(url);
    free(fp);
    free(tmo);
    return;

fail_free:
    free(url);
    free(fp);
    free(tmo);
    InterlockedExchange(&A.busy, 0);
    set_busy_ui(0);
}

void apitest_on_done(UINT_PTR gen, LPARAM lp)
{
    api_result *res = (api_result *)lp;

    if (!res)
        return;
    if (gen != A.gen) {              /* 过期结果：直接丢弃 */
        result_free(res);
        free(res);
        return;
    }

    A.last = res;
    InterlockedExchange(&A.busy, 0);
    set_busy_ui(0);
    render_result();

    if (!res->net_ok) {
        wchar_t line[1024];
        wset(line, 1024, L"请求失败：%s\n\n%s",
             res->fail_stage, res->fail_msg);
        MessageBoxW(main_window(), line, L"接口测试", MB_OK | MB_ICONERROR);
    }
}

int apitest_is_busy(void)
{
    return A.busy ? 1 : 0;
}

void apitest_on_close(void)
{
    /* 让可能还在跑的 worker 结果失效；worker 只 PostMessage，不会碰窗口 */
    A.gen++;
    if (A.last) {
        result_free(A.last);
        free(A.last);
        A.last = NULL;
    }
}

/* ==================================================================
 * 保存图片 / 保存 TXT
 * ================================================================== */

/*
 * 把结果里的图片存到 dir。
 * 只存 outputImages 与 markdown.images，跳过 inputImage
 * （inputImage 是原图回显，没有额外价值，还占空间）。
 *
 * 文件名固定为「时间戳 + 两位编号 + 魔数识别出的扩展名」，例如
 *   ocr_20261022_162013_01.png
 * 不再拼接响应里的 JSON 路径（如 layoutParsingResults[0].markdown.images.xxx）：
 * 那段路径很长、含 "." 和下标，拼进文件名后既难辨认，也容易碰到长度上限。
 */
static int save_images_to(const wchar_t *dir, int *okN, int *failN,
                          wchar_t *note, size_t cch)
{
    api_result *r = A.last;
    SYSTEMTIME st;
    size_t i;
    unsigned long long total = 0;
    wchar_t failDetail[1024];

    *okN = 0;
    *failN = 0;
    failDetail[0] = L'\0';

    if (!r || !r->net_ok || r->images.count == 0) {
        wset(note, cch, L"响应里没有可保存的图片");
        return -1;
    }
    if (r->images.count > A_MAX_IMAGES) {
        wset(note, cch, L"图片数量 %Iu 张，超过上限 %d 张，已中止",
             r->images.count, A_MAX_IMAGES);
        return -1;
    }

    GetLocalTime(&st);

    for (i = 0; i < r->images.count; i++) {
        const jl_image_ref *ref = &r->images.items[i];
        size_t plen = 0, cap, dn;
        const char *payload;
        unsigned char *buf;
        wchar_t name[256], path[1024], err[512], wext[16];
        int seq;

        /* 跳过 inputImage：它是原图回显，没有额外价值，还占空间 */
        if (strstr(ref->path, "inputImage") != NULL)
            continue;

        payload = jl_image_payload_ptr(r->http.data, ref, &plen);
        if (!payload || plen == 0) {
            (*failN)++;
            continue;
        }
        if (plen > A_MAX_ONE_IMAGE * 4 / 3 + 8) {
            (*failN)++;
            wset(failDetail, 1024, L"单张图片过大（Base64 %Iu 字符）", plen);
            continue;
        }

        cap = b64_decoded_cap(plen);
        buf = (unsigned char *)malloc(cap ? cap : 1);
        if (!buf) {
            (*failN)++;
            continue;
        }
        dn = b64_decode(payload, plen, buf, cap);
        if (dn == (size_t)-1 || dn == 0) {
            free(buf);
            (*failN)++;
            continue;
        }

        /*
         * 编号用"已处理张数 + 1"，跳过 inputImage 不会在序号里留下空洞。
         * 扩展名来自解码后的文件头（魔数），这里先转成宽字符再拼，
         * 避免 %s 把 char* 当 wchar_t* 解读、产出乱码后缀。
         */
        seq = (*okN) + (*failN) + 1;
        ascii_to_wide((ref->ext[0] == '.') ? ref->ext : ".bin", wext, 16);
        wset(name, 256, L"ocr_%04d%02d%02d_%02d%02d%02d_%02d%s",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             seq, wext);
        wset(path, 1024, L"%s\\%s", dir, name);
        if (A.ctx.unique_path)
            A.ctx.unique_path(path, 1024);

        if (A.ctx.write_file && A.ctx.write_file(path, buf, dn, err, 512)) {
            (*okN)++;
            total += dn;
        } else {
            (*failN)++;
            if (!failDetail[0])
                wset(failDetail, 1024, L"%s", err);
        }
        free(buf);
    }

    if (failDetail[0]) {
        wset(note, cch, L"保存 %d 张，失败 %d 张（%s）。共 %I64u 字节",
             *okN, *failN, failDetail, total);
    } else {
        wset(note, cch, L"保存 %d 张，失败 %d 张。共 %I64u 字节",
             *okN, *failN, total);
    }
    return (*okN > 0) ? 0 : -1;
}

static void do_save_images(void)
{
    wchar_t dir[1024], note[1024];
    int ok = 0, bad = 0;

    if (!A.last || !A.last->net_ok) {
        msg_error(L"还没有成功的响应，先点「发送请求」。");
        return;
    }
    if (A.last->images.count == 0) {
        msg_error(L"这次响应里没有识别到图片。\n\n"
                  L"如果服务端配置了不返回图片（例如 visualize=false），"
                  L"可以把这个开关关掉再试。");
        return;
    }

    wset(dir, 1024, L"%s", A.outdir_path);
    if (!A.ctx.pick_folder ||
        !A.ctx.pick_folder(main_window(), L"选择图片保存文件夹", dir, 1024))
        return;

    if (save_images_to(dir, &ok, &bad, note, 1024) == 0) {
        wset(A.outdir_path, 1024, L"%s", dir);
        SetWindowTextW(A.outdir, A.outdir_path);
        msg_info(L"%s\n\n保存到：\n%s", note, dir);
    } else {
        msg_error(L"没有成功保存任何图片。\n\n%s", note);
    }
}

/*
 * 生成「图片 Base64 已替换为 "..."」的副本。
 *
 * 预览、复制响应、保存 TXT 三者都走这一个函数，保证内容一致：
 * 用户看到的、复制的、保存的永远是脱敏文本；原始 Base64 只留在内存里
 * 供「保存图片」还原成图片文件，不再展示、不再落盘。
 */
static int make_redacted(const api_result *r, char **out, size_t *outLen)
{
    if (!r || !r->http.data || !out || !outLen)
        return -1;
    *out = NULL;
    *outLen = 0;
    return jl_redact(r->http.data, r->http.len, &r->images, out, outLen);
}

/* 某个「替换 base64」复选框是否勾选 */
static int redact_on(HWND chk)
{
    return (chk && SendMessageW(chk, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
}

/* 第 4 页的开关：作用于"保存为 TXT…"与"复制响应" */
static int redact_checked(void)
{
    return redact_on(A.redact);
}

static void do_save_txt(void)
{
    wchar_t path[1024], err[512];
    char *red = NULL;
    size_t redLen = 0;

    if (!A.last || !A.last->net_ok || !A.last->http.data) {
        msg_error(L"还没有成功的响应，先点「发送请求」。");
        return;
    }
    if (!A.ctx.pick_save_txt || !A.ctx.pick_save_txt(path, 1024, L"api_response.txt"))
        return;

    if (redact_checked()) {
        if (make_redacted(A.last, &red, &redLen) != 0) {
            msg_error(L"内存不足，无法生成脱敏副本。");
            return;
        }
        if (!A.ctx.write_file || !A.ctx.write_file(path, red, redLen, err, 512)) {
            msg_error(L"写入失败：\n%s", err);
            free(red);
            return;
        }
        free(red);
        msg_info(L"已保存（图片 base64 已替换为 \"...\"）：\n\n%s", path);
        return;
    }

    if (!A.ctx.write_file ||
        !A.ctx.write_file(path, A.last->http.data, A.last->http.len, err, 512)) {
        msg_error(L"写入失败：\n%s", err);
        return;
    }
    msg_info(L"已保存完整响应（含 base64）：\n\n%s", path);
}

/* ==================================================================
 * 消息处理
 * ================================================================== */

/*
 * 参数区的勾选/输入消息。每个页有自己的 id_base，所以两页各调各的。
 * 返回 1 表示这条消息属于参数区，已处理完。
 */
static int params_on_command(param_ui *p, int id, int code)
{
    int off, idx;

    if (!p || !p->built || id < p->id_base || id >= p->id_base + 200)
        return 0;

    off = id - p->id_base;
    idx = off / 2;
    if ((off % 2) == 0 && idx < (int)api_param_count() && p->val[idx] &&
        code == BN_CLICKED) {
        int on = (SendMessageW(p->chk[idx], BM_GETCHECK, 0, 0) == BST_CHECKED);
        EnableWindow(p->val[idx], on ? TRUE : FALSE);
        if (on)
            SetFocus(p->val[idx]);
    }
    return 1;
}

int apitest_on_command(HWND hMain, WORD id, WORD code)
{
    (void)hMain;

    if (params_on_command(&A.params, (int)id, (int)code))
        return 1;

    switch (id) {
    case IDA_PICK_FILE:
        if (code == BN_CLICKED) {
            wchar_t path[1024];

            if (A.ctx.pick_file && A.ctx.pick_file(path, 1024))
                set_source(path);
        }
        return 1;

    case IDA_SEND:
        if (code == BN_CLICKED)
            do_send();
        return 1;

    case IDA_CLEAR:
        if (code == BN_CLICKED) {
            SetWindowTextW(A.file_edit, L"");
            SetWindowTextW(A.resp, L"");
            SetWindowTextW(A.meta1, L"尚未发送请求。");
            SetWindowTextW(A.meta2, L"");
            SetWindowTextW(A.hint, A_HINT_TEXT);
            A.meta2_bad = 0;
            InvalidateRect(A.meta2, NULL, TRUE);
            if (A.last) {
                result_free(A.last);
                free(A.last);
                A.last = NULL;
            }
        }
        return 1;

    case IDA_COPY_RESP:
        if (code == BN_CLICKED) {
            if (!A.last || !A.last->http.data) {
                msg_error(L"还没有响应内容可复制。");
                return 1;
            }
            if (redact_checked()) {
                char *red = NULL;
                size_t redLen = 0;

                if (make_redacted(A.last, &red, &redLen) != 0) {
                    msg_error(L"内存不足，无法生成脱敏副本。");
                    return 1;
                }
                copy_clip(red, redLen);
                free(red);
                SetWindowTextW(A.meta2,
                               L"响应已复制到剪贴板（图片 base64 已替换为 \"...\"）。");
            } else {
                copy_clip(A.last->http.data, A.last->http.len);
                SetWindowTextW(A.meta2, L"完整响应已复制到剪贴板（含 base64）。");
            }
        }
        return 1;

    case IDA_BROWSE:
        if (code == BN_CLICKED) {
            wchar_t dir[1024];

            wset(dir, 1024, L"%s", A.outdir_path);
            if (A.ctx.pick_folder &&
                A.ctx.pick_folder(main_window(), L"选择保存文件夹", dir, 1024)) {
                wset(A.outdir_path, 1024, L"%s", dir);
                SetWindowTextW(A.outdir, A.outdir_path);
            }
        }
        return 1;

    case IDA_SAVE_IMG:
        if (code == BN_CLICKED)
            do_save_images();
        return 1;

    case IDA_SAVE_TXT:
        if (code == BN_CLICKED)
            do_save_txt();
        return 1;

    default:
        break;
    }

    return 0;
}

static void params_vscroll(param_ui *p, WPARAM wp)
{
    int code = LOWORD(wp);
    int track;

    if (!p->vsb)
        return;
    switch (code) {
    case SB_LINEUP:     p->scroll_pos -= A.ctx.ds(24); break;
    case SB_LINEDOWN:   p->scroll_pos += A.ctx.ds(24); break;
    case SB_PAGEUP:     p->scroll_pos -= p->view_h; break;
    case SB_PAGEDOWN:   p->scroll_pos += p->view_h; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION:
        track = HIWORD(wp);
        if (track == 0xFFFF) {
            SCROLLINFO si;
            memset(&si, 0, sizeof(si));
            si.cbSize = sizeof(si);
            si.fMask = SIF_TRACKPOS;
            GetScrollInfo(p->vsb, SB_CTL, &si);
            p->scroll_pos = si.nTrackPos;
        } else {
            p->scroll_pos = track;
        }
        break;
    case SB_TOP:        p->scroll_pos = 0; break;
    case SB_BOTTOM:     p->scroll_pos = p->scroll_max; break;
    default:
        return;
    }
    params_update_scrollbar(p);
    params_apply_scroll(p);
}

void apitest_on_vscroll(WPARAM wp)
{
    params_vscroll(&A.params, wp);
}

static void params_wheel(param_ui *p, int delta)
{
    if (p->scroll_max <= 0)
        return;
    p->scroll_pos -= (delta / WHEEL_DELTA) * A.ctx.ds(60);
    params_update_scrollbar(p);
    params_apply_scroll(p);
}

void apitest_on_wheel(int delta)
{
    params_wheel(&A.params, delta);
}

static int params_hit(param_ui *p)
{
    POINT pt;
    RECT r;

    if (!p->host || !IsWindow(p->host) || !IsWindowVisible(p->host))
        return 0;
    GetCursorPos(&pt);
    GetWindowRect(p->host, &r);
    if (PtInRect(&r, pt))
        return 1;
    if (p->vsb && IsWindowVisible(p->vsb)) {
        GetWindowRect(p->vsb, &r);
        if (PtInRect(&r, pt))
            return 1;
    }
    return 0;
}

int apitest_wants_wheel(void)
{
    return params_hit(&A.params);
}

void apitest_on_dropfiles(HDROP drop)
{
    wchar_t path[1024];
    UINT got;

    if (!drop)
        return;
    got = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
    if (got == 1) {
        path[0] = L'\0';
        if (DragQueryFileW(drop, 0, path, 1024) > 0)
            set_source(path);
    } else if (got > 1) {
        msg_error(L"这一页一次只处理一个文件，已忽略其余 %u 个。", got - 1);
        path[0] = L'\0';
        if (DragQueryFileW(drop, 0, path, 1024) > 0)
            set_source(path);
    }
    DragFinish(drop);
}

int apitest_ctlcolor(HWND ctl, HDC dc, LRESULT *out)
{
    if (!out)
        return 0;
    if (ctl == A.meta1 || ctl == A.meta2 || ctl == A.hint) {
        SetBkMode(dc, TRANSPARENT);
        if (ctl == A.meta2 && A.meta2_bad)
            SetTextColor(dc, RGB(180, 24, 24));
        else if (ctl == A.hint)
            SetTextColor(dc, RGB(70, 70, 70));
        else
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        *out = (LRESULT)A.ctx.br_panel;
        return 1;
    }
    return 0;
}

/* ==================================================================
 * /diag 几何输出
 * ================================================================== */

static void diag_line(wchar_t *buf, size_t cch, const wchar_t *name, HWND h)
{
    wchar_t tmp[256];
    size_t used = wcslen(buf);
    RECT r;
    int x = -1, y = -1, w = -1, ht = -1, vis = -1;

    if (h && IsWindow(h)) {
        POINT pts[2];

        if (GetWindowRect(h, &r)) {
            pts[0].x = r.left;  pts[0].y = r.top;
            pts[1].x = r.right; pts[1].y = r.bottom;
            MapWindowPoints(NULL, main_window(), pts, 2);
            x = (int)pts[0].x;
            y = (int)pts[0].y;
            w = (int)(pts[1].x - pts[0].x);
            ht = (int)(pts[1].y - pts[0].y);
        }
        vis = IsWindowVisible(h) ? 1 : 0;
    }

    if (!h || !IsWindow(h))
        wset(tmp, 256, L"%-16s <未创建>\r\n", name);
    else
        wset(tmp, 256, L"%-16s (%d,%d) %dx%d 可见=%d\r\n", name, x, y, w, ht, vis);

    if (used + wcslen(tmp) + 1 < cch)
        wcsncat(buf, tmp, cch - used - 1);
}

void apitest_diag(wchar_t *buf, size_t cch)
{
    wchar_t l[256];
    size_t used;

    if (!buf || !A.built)
        return;

    wset(l, 256, L"\r\n--- 第 4 页（接口测试）---\r\n");
    used = wcslen(buf);
    if (used + wcslen(l) + 1 < cch)
        wcsncat(buf, l, cch - used - 1);

    diag_line(buf, cch, L"URL框", A.url);
    diag_line(buf, cch, L"方法", A.method);
    diag_line(buf, cch, L"发送", A.send);
    diag_line(buf, cch, L"源文件框", A.file_edit);
    diag_line(buf, cch, L"选择文件", A.pick);
    diag_line(buf, cch, L"fileType", A.ftype);
    diag_line(buf, cch, L"参数宿主", A.params.host);
    diag_line(buf, cch, L"滚动条", A.params.vsb);
    diag_line(buf, cch, L"meta1", A.meta1);
    diag_line(buf, cch, L"响应框", A.resp);
    diag_line(buf, cch, L"保存图片", A.save_img);

    wset(l, 256, L"参数区：内容高 %d，可视高 %d，滚动上限 %d，当前位置 %d\r\n",
         A.params.content_h, A.params.view_h, A.params.scroll_max, A.params.scroll_pos);
    used = wcslen(buf);
    if (used + wcslen(l) + 1 < cch)
        wcsncat(buf, l, cch - used - 1);
}

/* ==================================================================
 * 命令行端到端自检：/api-test
 *
 * 存在的理由：开发机跑不了 Windows 界面。这条命令能在目标机上把
 * 「读文件 -> 识别类型 -> Base64 -> 组装 body -> 发请求 -> 解析 -> 存图」
 * 整条链路跑一遍并逐段给结论，出问题时能直接指出是第几步、什么原因。
 * ================================================================== */

static const wchar_t *a_cmdline_args(void)
{
    const wchar_t *cl = GetCommandLineW();
    const wchar_t *p;

    if (!cl)
        return NULL;
    p = cl;
    if (*p == L'"') {
        p = wcschr(p + 1, L'"');
        if (p)
            p++;
    } else {
        while (*p && *p != L' ' && *p != L'\t')
            p++;
    }
    return (p && *p) ? p : NULL;
}

static int a_next_token(const wchar_t **pp, wchar_t *out, size_t cch)
{
    const wchar_t *p = *pp;
    size_t n = 0;

    if (!p || cch == 0)
        return 0;
    while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n')
        p++;
    if (!*p) {
        *pp = p;
        return 0;
    }
    if (*p == L'"') {
        p++;
        while (*p && *p != L'"' && n + 1 < cch)
            out[n++] = *p++;
        if (*p == L'"')
            p++;
    } else {
        while (*p && *p != L' ' && *p != L'\t' && *p != L'\r' &&
               *p != L'\n' && n + 1 < cch)
            out[n++] = *p++;
    }
    out[n] = L'\0';
    *pp = p;
    return 1;
}

static void rep(wchar_t *out, size_t cch, size_t *used, const wchar_t *fmt, ...)
{
    va_list ap;
    int n;

    if (!out || *used >= cch - 1)
        return;
    va_start(ap, fmt);
    n = _vsnwprintf(out + *used, cch - *used - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    *used += (size_t)n;
    if (*used >= cch)
        *used = cch - 1;
    out[*used] = L'\0';
}

static int a_find_param(const char *name)
{
    size_t i;

    for (i = 0; i < api_param_count(); i++) {
        const api_param_def *d = api_param_at(i);
        if (d && strcmp(d->name, name) == 0)
            return (int)i;
    }
    return -1;
}

/* "name=value"：布尔用 true/false，其它直接写入值 */
static int a_apply_param(api_param_state *ps, const wchar_t *spec)
{
    const wchar_t *eq = wcschr(spec, L'=');
    char name[64];
    char value[API_VALUE_MAX];
    size_t i, n = 0;
    int idx;
    char *u;

    if (!eq || eq == spec)
        return 0;
    for (i = 0; i < (size_t)(eq - spec) && i + 1 < sizeof(name); i++)
        name[i] = (char)spec[i];
    name[i] = '\0';
    idx = a_find_param(name);
    if (idx < 0)
        return 0;

    u = a_wide_to_utf8(eq + 1, NULL);
    if (!u)
        return 0;
    snprintf(value, sizeof(value), "%s", u);
    free(u);

    {
        const api_param_def *d = api_param_at((size_t)idx);
        if (d->type == AP_BOOL) {
            ps[idx].on = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) ? 1 : 0;
        } else {
            snprintf(ps[idx].value, API_VALUE_MAX, "%s", value);
            ps[idx].on = 1;
        }
    }
    (void)n;
    return 1;
}

static void cli_default_outdir(wchar_t *out, size_t cch)
{
    wchar_t p[1024];
    wchar_t *s;

    GetModuleFileNameW(NULL, p, 1024);
    s = wcsrchr(p, L'\\');
    if (s)
        *s = L'\0';
    wset(out, cch, L"%s", p);
}

int handle_api_test(void)
{
    const wchar_t *args = a_cmdline_args();
    const wchar_t *p;
    wchar_t tok[2048];
    wchar_t file[1024] = L"";
    wchar_t url[2048] = L"";
    wchar_t outdir[1024] = L"";
    wchar_t reportPath[1024] = L"";
    int method_get = 0, use_proxy = 0, dry = 0, save = 0;
    int file_type = -1;
    int got_file = 0;
    DWORD timeout_ms = 180000;
    api_param_state ps[API_MAX_PARAMS];
    api_job *job;
    api_result *res;
    wchar_t buf[32768];
    size_t used = 0;

    if (!args || !wcsstr(args, L"/api-test"))
        return 0;

    if (!A.ctx.read_file) {              /* 宿主能力还没注入（界面没建起来） */
        MessageBoxW(NULL, L"界面未初始化，无法执行 /api-test。", L"接口测试",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    api_params_default(ps);

    memset(tok, 0, sizeof(tok));      /* 让扫描器看到完整初始化的缓冲 */
    p = wcsstr(args, L"/api-test") + 9;
    while (a_next_token(&p, tok, 2048)) {
        if (tok[0] == L'/') {
            if (wcsncmp(tok, L"/url:", 5) == 0)
                wset(url, 2048, L"%s", tok + 5);
            else if (wcscmp(tok, L"/method:GET") == 0)
                method_get = 1;
            else if (wcscmp(tok, L"/method:POST") == 0)
                method_get = 0;
            else if (wcscmp(tok, L"/dry") == 0)
                dry = 1;
            else if (wcscmp(tok, L"/save") == 0)
                save = 1;
            else if (wcscmp(tok, L"/proxy") == 0)
                use_proxy = 1;
            else if (wcsncmp(tok, L"/outdir:", 8) == 0)
                wset(outdir, 1024, L"%s", tok + 8);
            else if (wcsncmp(tok, L"/report:", 8) == 0)
                wset(reportPath, 1024, L"%s", tok + 8);
            else if (wcsncmp(tok, L"/timeout:", 9) == 0) {
                int s = (int)wcstol(tok + 9, NULL, 10);
                if (s < 5) s = 5;
                if (s > 3600) s = 3600;
                timeout_ms = (DWORD)s * 1000u;
            } else if (wcsncmp(tok, L"/filetype:", 10) == 0) {
                if (_wcsicmp(tok + 10, L"auto") == 0)
                    file_type = -1;
                else
                    file_type = (wcstol(tok + 10, NULL, 10) == 0) ? 0 : 1;
            } else if (wcsncmp(tok, L"/param:", 7) == 0) {
                a_apply_param(ps, tok + 7);
            } else if (wcsncmp(tok, L"/raw:", 5) == 0) {
                a_apply_param(ps, tok + 5);
            }
        } else if (!got_file) {
            wset(file, 1024, L"%s", tok);
            got_file = 1;
        }
    }

    memset(buf, 0, sizeof(buf));

    if (!got_file || !file[0]) {
        rep(buf, 32768, &used,
            L"[/api-test 用法]\r\n"
            L"  B64ShaTool_x64.exe /api-test \"<文件路径>\" /url:http://ip:port/layout-parsing\r\n"
            L"        [/method:GET|POST] [/filetype:auto|0|1] [/timeout:180]\r\n"
            L"        [/param:useLayoutDetection=true] [/param:layoutThreshold=0.3]\r\n"
            L"        [/raw:markdownIgnoreLabels=[\"number\"]] [/proxy]\r\n"
            L"        [/outdir:D:\\输出目录] [/save] [/dry] [/report:D:\\report.txt]\r\n");
        MessageBoxW(NULL, buf, L"接口测试自检", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    if (!url[0]) {
        rep(buf, 32768, &used, L"缺少 /url: 参数，例如 /url:http://10.1.2.3:8080/layout-parsing\r\n");
        MessageBoxW(NULL, buf, L"接口测试自检", MB_OK | MB_ICONWARNING);
        return 1;
    }
    if (!outdir[0])
        cli_default_outdir(outdir, 1024);

    job = (api_job *)calloc(1, sizeof(*job));
    res = (api_result *)calloc(1, sizeof(*res));
    if (!job || !res) {
        free(job);
        free(res);
        MessageBoxW(NULL, L"内存不足。", L"接口测试自检", MB_OK | MB_ICONERROR);
        return 1;
    }
    wset(job->path, 1024, L"%s", file);
    wset(job->url, 2048, L"%s", url);
    job->method_get = method_get;
    job->file_type = file_type;
    job->use_proxy = use_proxy;
    job->timeout_ms = timeout_ms;
    job->dry = dry;
    memcpy(job->params, ps, sizeof(ps));

    {
        int rc = api_run(job, res);

        rep(buf, 32768, &used,
            L"[/api-test 接口链路自检]\r\n"
            L"源文件   : %s\r\n"
            L"目标 URL : %s   方法 %s%s\r\n\r\n",
            file, url, method_get ? L"GET" : L"POST", dry ? L"（dry：不联网）" : L"");

        /* [1] 读文件 */
        if (res->file_size > 0 || (rc == 0 && res->b64_len > 0)) {
            rep(buf, 32768, &used, L"[1] 读取文件 ......... %I64u 字节（%I64u ms）→ OK\r\n",
                res->file_size, res->t_read_ms);
        }
        /* [2] 类型 */
        if (res->ext[0]) {
            rep(buf, 32768, &used,
                L"[2] 类型识别 ......... %S → fileType=%d%s → OK\r\n",
                res->ext, res->file_type, res->ext_known ? L"" : L"（未收录，按图片处理）");
        }
        /* [3] Base64 */
        if (res->b64_len > 0) {
            rep(buf, 32768, &used,
                L"[3] Base64 编码 ...... %Iu 字符（%I64u ms）→ OK\r\n",
                res->b64_len, res->t_b64_ms);
        }
        /* [4] body */
        if (rc == 0 || res->body_len > 0) {
            rep(buf, 32768, &used,
                L"[4] 组装 Body ........ %Iu 字节 → OK\r\n    SHA-256: %.16S…\r\n",
                res->body_len, res->body_sha);
        }
        if (res->net_ok && !dry) {
            wchar_t sz[64];
            size_text((unsigned long long)res->http.len, sz, 64);
            rep(buf, 32768, &used,
                L"[5] 发送请求 ......... HTTP %d %s，用时 %I64u ms，响应 %s → OK\r\n",
                res->http.http_status,
                res->http.status_text[0] ? res->http.status_text : L"",
                res->http.elapsed_ms, sz);
        }
        if (res->has_biz) {
            rep(buf, 32768, &used, L"[6] 解析响应 ......... errorCode=%ld %s → %s\r\n",
                res->biz_code, res->biz_msg,
                res->biz_code == 0 ? L"OK" : L"业务返回了错误");
        }
        if (res->net_ok && !dry) {
            size_t cnt = 0, k;
            for (k = 0; k < res->images.count; k++) {
                if (strstr(res->images.items[k].path, "inputImage") == NULL)
                    cnt++;
            }
            rep(buf, 32768, &used,
                L"[7] 提取图片 ......... %Iu 张可保存（已排除 inputImage）→ OK\r\n", cnt);
        }

        /* [8] 保存图片 */
        if (save && res->net_ok && !dry) {
            wchar_t note[1024];
            int ok = 0, bad = 0;

            A.last = res;                 /* save_images_to 从 A.last 取 */
            if (save_images_to(outdir, &ok, &bad, note, 1024) == 0) {
                    rep(buf, 32768, &used, L"[8] 保存图片 ......... %s\r\n    目录：%s\r\n",
                    note, outdir);
            } else {
                rep(buf, 32768, &used, L"[8] 保存图片 ......... 未保存成功：%s\r\n", note);
            }
            A.last = NULL;
        } else if (save) {
            rep(buf, 32768, &used, L"[8] 保存图片 ......... 跳过（dry 或请求失败）\r\n");
        }

        /* [9] 脱敏副本 */
        if (res->net_ok && !dry && res->http.data) {
            char *red = NULL;
            size_t redLen = 0;

            if (jl_redact(res->http.data, res->http.len, &res->images,
                          &red, &redLen) == 0) {
                    rep(buf, 32768, &used,
                    L"[9] 脱敏副本 ......... %I64u → %Iu 字节 → OK\r\n",
                    (unsigned long long)res->http.len, redLen);
                free(red);
            } else {
                rep(buf, 32768, &used, L"[9] 脱敏副本 ......... 内存不足，跳过\r\n");
            }
        }

        /* 结论 */
        if (rc != 0) {
            rep(buf, 32768, &used, L"\r\n结论: 在第 [%s] 步失败 —— %s\r\n",
                res->fail_stage, res->fail_msg);
        } else if (res->has_biz && res->biz_code != 0) {
            rep(buf, 32768, &used,
                L"\r\n结论: 链路本身正常，但服务端返回业务错误 errorCode=%ld —— %s\r\n",
                res->biz_code, res->biz_msg);
        } else if (dry) {
            rep(buf, 32768, &used,
                L"\r\n结论: dry 模式通过 —— 读取、类型识别、Base64、组装请求体均正常\r\n");
        } else {
            rep(buf, 32768, &used,
                L"\r\n结论: 全部通过 —— 该文件到该接口的整条链路正常\r\n");
        }

        if (reportPath[0]) {
            size_t u8n = 0;
            char *u8 = a_wide_to_utf8(buf, &u8n);
            wchar_t werr[512];

            if (u8 && A.ctx.write_file) {
                if (A.ctx.write_file(reportPath, u8, u8n, werr, 512))
                    rep(buf, 32768, &used, L"报告已写入：%s\r\n", reportPath);
                else
                    rep(buf, 32768, &used, L"报告写入失败：%s\r\n", werr);
            }
            free(u8);
        }

        {
            size_t u8n = 0;
            char *u8 = a_wide_to_utf8(buf, &u8n);
            copy_clip(u8, u8n);
            free(u8);
        }

        MessageBoxW(NULL, buf, L"接口测试自检（内容已复制到剪贴板）",
                    MB_OK | (rc == 0 ? MB_ICONINFORMATION : MB_ICONERROR));
    }

    result_free(res);
    free(res);
    free(job);
    return 1;
}

/* ==================================================================
 * 第 5 页：批量接口测试
 *
 * 与第 4 页的区别只有两点：
 *   1) 源从"一个文件"变成"一个文件夹"，按扩展名筛出 PDF 与图片；
 *   2) 结果不逐张展示，只往表格里填一行（名称/大小/类型/图片数/状态）。
 * 参数区、请求体、保存规则全部复用第 4 页的同一套实现。
 *
 * 执行模型：串行。OCR 请求动辄几十秒，并发会把内网服务和本机内存同时压满；
 * 串行还有一个好处是进度和错误都能对上具体文件。可以随时"停止"，
 * 正在飞的那一个请求会等它返回（WinHTTP 同步调用无法安全中断）。
 * ================================================================== */

/* 给 STATIC 控件设置格式化文本（wset 是给 wchar 缓冲用的，不能直接传 HWND） */
static void set_static(HWND h, const wchar_t *fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;

    if (!h)
        return;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1023, fmt, ap);
    va_end(ap);
    buf[1023] = L'\0';
    SetWindowTextW(h, buf);
}

#define BATCH_MAX_FILES  300
/*
 * 整批最多在内存里保留多少响应体。超过后继续跑，但会释放已收响应，
 * 只保留统计（表格照常显示），避免服务端返回大图时把内存吃穿。
 */
#define BATCH_RESP_BUDGET ((unsigned long long)512 * 1024 * 1024)

typedef struct {
    wchar_t            path[1024];
    wchar_t            name[260];        /* 文件名（含扩展名） */
    unsigned long long size;
    api_result         res;              /* 完整响应（供保存图片/文本） */
    int                done;             /* 1 = 请求已结束 */
    int                images;           /* 可保存图片数（已排除 inputImage） */
    int                trimmed;          /* 1 = 响应体因内存预算已释放 */
} batch_row;

typedef struct {
    int      built;
    HWND     lbl_url, url, method, send, stop, clear;
    HWND     lbl_dir, dir_edit, pick_dir, sel_all, sel_none;
    HWND     proxy, timeout, lbl_sec, hint;
    param_ui params;
    HWND     list, meta;
    HWND     lbl_out, outdir, browse, save_img, save_txt, redact;

    batch_row *rows;
    int        count, cap;

    int        running;
    LONG       stop_flag;
    UINT_PTR   gen;

    wchar_t    outdir_path[1024];
} batch_ui;

static batch_ui B;
static unsigned long long B_resp_bytes;   /* 本轮已保留的响应体总字节 */

/* 只收这几类：PDF 与常见位图。其余文件跳过并在状态里说明 */
static int batch_is_target(const wchar_t *name)
{
    static const wchar_t *OK[] = {
        L".pdf", L".png", L".jpg", L".jpeg", L".bmp",
        L".tif", L".tiff", L".webp", L".gif", NULL
    };
    const wchar_t *dot = wcsrchr(name, L'.');
    int i;

    if (!dot || !dot[1])
        return 0;
    for (i = 0; OK[i]; i++)
        if (_wcsicmp(dot, OK[i]) == 0)
            return 1;
    return 0;
}

static int batch_cmp_name(const void *a, const void *b)
{
    const batch_row *x = (const batch_row *)a;
    const batch_row *y = (const batch_row *)b;
    return _wcsicmp(x->name, y->name);
}

/* 释放全部行（响应数据 + 数组本身） */
static void batch_clear_rows(void)
{
    int i;

    if (!B.rows)
        return;
    for (i = 0; i < B.count; i++)
        result_free(&B.rows[i].res);
    free(B.rows);
    B.rows = NULL;
    B.count = 0;
    B.cap = 0;
    B_resp_bytes = 0;
    if (B.list)
        SendMessageW(B.list, LVM_DELETEALLITEMS, 0, 0);
}

/*
 * 枚举文件夹（不递归）里的 PDF 与图片，按文件名排序。
 * 返回收录的文件数；0 表示没有可用文件（原因写进 err）。
 */
static int batch_collect(const wchar_t *dir, wchar_t *err, size_t errc)
{
    WIN32_FIND_DATAW fd;
    wchar_t pattern[1024];
    HANDLE h;
    int skipped = 0;

    batch_clear_rows();
    if (err && errc)
        err[0] = L'\0';

    wset(pattern, 1024, L"%s\\*", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        if (err && errc)
            wset(err, errc, L"无法读取文件夹（错误码 %lu）", GetLastError());
        return 0;
    }

    do {
        batch_row *row;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (!batch_is_target(fd.cFileName)) {
            skipped++;
            continue;
        }
        if (B.count >= BATCH_MAX_FILES) {
            skipped++;
            continue;
        }
        if (B.count >= B.cap) {
            int want = B.cap ? B.cap * 2 : 32;
            batch_row *p = (batch_row *)realloc(B.rows, (size_t)want * sizeof(batch_row));
            if (!p) {
                FindClose(h);
                if (err && errc)
                    wset(err, errc, L"内存不足，无法容纳更多文件");
                return B.count;
            }
            memset(p + B.cap, 0, (size_t)(want - B.cap) * sizeof(batch_row));
            B.rows = p;
            B.cap = want;
        }

        row = &B.rows[B.count];
        memset(row, 0, sizeof(*row));
        wset(row->path, 1024, L"%s\\%s", dir, fd.cFileName);
        wset(row->name, 260, L"%s", fd.cFileName);
        row->size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        B.count++;
    } while (FindNextFileW(h, &fd));

    FindClose(h);

    if (B.count > 1)
        qsort(B.rows, (size_t)B.count, sizeof(batch_row), batch_cmp_name);

    if (B.count == 0 && err && errc)
        wset(err, errc,
             L"这个文件夹里没有 PDF 或图片文件（跳过 %d 个其他文件）", skipped);
    return B.count;
}

/* 源文件名 -> 可安全用作文件名的"主名"（去扩展名、滤非法字符、限制长度） */
static void batch_stem(const wchar_t *name, wchar_t *out, size_t cch)
{
    const wchar_t *dot = wcsrchr(name, L'.');
    size_t i, n = 0;

    if (!out || cch == 0)
        return;
    for (i = 0; name[i] && n + 1 < cch && n < 48; i++) {
        wchar_t c;

        if (dot && &name[i] >= dot)
            break;
        c = name[i];
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c))
            c = L'_';
        out[n++] = c;
    }
    out[n] = L'\0';
    if (!out[0])
        wset(out, cch, L"file");
}

/* ListView 的复选框状态 */
static int batch_list_checked(int i)
{
    return (ListView_GetItemState(B.list, i, LVIS_STATEIMAGEMASK) >> 12) == 2 ? 1 : 0;
}

static int batch_selected_count(void)
{
    int i, n = 0;

    for (i = 0; i < B.count; i++)
        if (batch_list_checked(i))
            n++;
    return n;
}

/* ---------- 表格 ---------- */

static void batch_list_col(int idx, const wchar_t *title, int cx)
{
    LVCOLUMNW c;

    memset(&c, 0, sizeof(c));
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.pszText = (LPWSTR)title;
    c.cx = cx;
    c.iSubItem = idx;
    SendMessageW(B.list, LVM_INSERTCOLUMNW, (WPARAM)idx, (LPARAM)&c);
}

static void batch_list_set(int row, int col, const wchar_t *text)
{
    LVITEMW it;

    memset(&it, 0, sizeof(it));
    it.iSubItem = col;
    it.pszText = (LPWSTR)text;
    SendMessageW(B.list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
}

/* 类型一栏给人看的是"PDF / 图片"，不是扩展名：fileType 只有 0 和 1 两种 */
static void batch_type_text(const char *ext, wchar_t *out, size_t cch)
{
    if (ext && _stricmp(ext, ".pdf") == 0)
        wset(out, cch, L"PDF");
    else if (ext && ext[0])
        wset(out, cch, L"图片");
    else
        wset(out, cch, L"-");
}

/* 用时单独一列：小于 1 秒按毫秒显示，避免一律 "0.0 s" 看不出差别 */
static void batch_dur_text(unsigned long long ms, wchar_t *out, size_t cch)
{
    if (ms == 0) {
        wset(out, cch, L"-");
        return;
    }
    if (ms < 1000)
        wset(out, cch, L"%I64u ms", ms);
    else
        wset(out, cch, L"%.1f s", (double)ms / 1000.0);
}

/* 把某一行的显示重置为初始状态；**不动勾选**，供"重新跑一批"时使用 */
static void batch_list_reset_row(int i)
{
    const wchar_t *dot;
    wchar_t sz[64];

    if (!B.list || i < 0 || i >= B.count)
        return;
    size_text(B.rows[i].size, sz, sizeof(sz) / sizeof(sz[0]));
    batch_list_set(i, 1, sz);

    dot = wcsrchr(B.rows[i].name, L'.');
    batch_list_set(i, 2, (dot && _wcsicmp(dot, L".pdf") == 0) ? L"PDF" : L"图片");
    batch_list_set(i, 3, L"-");
    batch_list_set(i, 4, L"-");
    batch_list_set(i, 5, L"待发送");
}

/* 按 B.rows 重建整个表格：枚举完文件夹时调用（会清空勾选） */
static void batch_list_fill(void)
{
    int i;

    if (!B.list)
        return;
    SendMessageW(B.list, LVM_DELETEALLITEMS, 0, 0);

    for (i = 0; i < B.count; i++) {
        LVITEMW it;

        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.iSubItem = 0;
        it.pszText = (LPWSTR)B.rows[i].name;
        it.lParam = i;
        SendMessageW(B.list, LVM_INSERTITEMW, 0, (LPARAM)&it);

        batch_list_reset_row(i);
    }
}

static void batch_list_update_row(int i)
{
    batch_row *row;
    wchar_t sz[64], line[512], dur[64], typ[16];

    if (!B.list || i < 0 || i >= B.count)
        return;
    row = &B.rows[i];

    size_text(row->size, sz, sizeof(sz) / sizeof(sz[0]));
    batch_list_set(i, 1, sz);

    batch_type_text(row->done ? row->res.ext : NULL, typ, 16);
    batch_list_set(i, 2, typ);

    {
        wchar_t num[32];
        if (row->done) {
            wset(num, 32, L"%d", row->images);
            batch_list_set(i, 3, num);
        }
    }

    batch_dur_text(row->done ? row->res.t_total_ms : 0, dur, 64);
    batch_list_set(i, 4, dur);

    if (!row->done) {
        batch_list_set(i, 5, L"请求中…");
        return;
    }

    if (!row->res.net_ok) {
        wset(line, 512, L"失败：%s / %s",
             row->res.fail_stage[0] ? row->res.fail_stage : L"未知阶段",
             row->res.fail_msg[0] ? row->res.fail_msg : L"无详细信息");
    } else if (row->res.has_biz && row->res.biz_code != 0) {
        wset(line, 512, L"errorCode=%ld：%s",
             row->res.biz_code,
             row->res.biz_msg[0] ? row->res.biz_msg : L"（响应无 errorMsg）");
    } else if (row->res.images.count > 0 && row->images == 0) {
        wset(line, 512, L"完成（响应里只有 inputImage，不可保存）");
    } else {
        wset(line, 512, L"完成");
    }

    if (row->trimmed) {
        wchar_t t[512];
        wset(t, 512, L"%s　·　响应体已释放（累计超过 512MB）", line);
        wset(line, 512, L"%s", t);
    }
    batch_list_set(i, 5, line);
}

/* ---------- 界面 ---------- */

int batchtest_build(HWND panel, const api_ui_ctx *ctx)
{
    static const wchar_t *METHODS[] = { L"POST", L"GET" };
    int i;

    if (!panel || !IsWindow(panel) || !ctx)
        return -1;
    if (B.built)
        return 0;
    B.built = 1;

    B.lbl_url = ctx->mk(panel, L"STATIC", L"接口地址:", SS_LEFT, 0, 0, ctx->font_ui);
    B.url = ctx->mk(panel, L"EDIT", L"http://127.0.0.1:8080/layout-parsing",
                    ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, IDB_URL, ctx->font_ui);
    B.method = ctx->mk(panel, L"COMBOBOX", L"",
                       CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0,
                       IDB_METHOD, ctx->font_ui);
    for (i = 0; i < 2; i++)
        SendMessageW(B.method, CB_ADDSTRING, 0, (LPARAM)METHODS[i]);
    SendMessageW(B.method, CB_SETCURSEL, 0, 0);

    B.send = ctx->mk(panel, L"BUTTON", L"开始批量", BS_PUSHBUTTON | WS_TABSTOP, 0,
                     IDB_SEND, ctx->font_ui);
    B.stop = ctx->mk(panel, L"BUTTON", L"停止", BS_PUSHBUTTON | WS_TABSTOP, 0,
                     IDB_STOP, ctx->font_ui);
    if (B.stop)
        EnableWindow(B.stop, FALSE);
    B.clear = ctx->mk(panel, L"BUTTON", L"清空", BS_PUSHBUTTON | WS_TABSTOP, 0,
                      IDB_CLEAR, ctx->font_ui);

    B.lbl_dir = ctx->mk(panel, L"STATIC", L"源文件夹:", SS_LEFT, 0, 0, ctx->font_ui);
    B.dir_edit = ctx->mk(panel, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP,
                         WS_EX_CLIENTEDGE, IDB_DIR_EDIT, ctx->font_ui);
    B.pick_dir = ctx->mk(panel, L"BUTTON", L"选择文件夹…", BS_PUSHBUTTON | WS_TABSTOP, 0,
                         IDB_PICK_DIR, ctx->font_ui);
    /* 类型不再手选：批量时逐个文件按内容（魔数）判断 PDF / 图片，自动填 fileType */
    B.sel_all = ctx->mk(panel, L"BUTTON", L"全选", BS_PUSHBUTTON | WS_TABSTOP, 0,
                        IDB_SEL_ALL, ctx->font_ui);
    B.sel_none = ctx->mk(panel, L"BUTTON", L"全不选", BS_PUSHBUTTON | WS_TABSTOP, 0,
                         IDB_SEL_NONE, ctx->font_ui);

    B.proxy = ctx->mk(panel, L"BUTTON", L"使用系统代理", BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                      IDB_PROXY, ctx->font_ui);
    B.timeout = ctx->mk(panel, L"EDIT", L"10", ES_AUTOHSCROLL | ES_NUMBER | WS_TABSTOP,
                        WS_EX_CLIENTEDGE, IDB_TIMEOUT, ctx->font_ui);
    B.lbl_sec = ctx->mk(panel, L"STATIC", L"秒超时", SS_LEFT, 0, 0, ctx->font_ui);
    B.hint = ctx->mk(panel, L"STATIC", L"", SS_LEFTNOWORDWRAP, 0, 0, ctx->font_ui);

    if (params_build(&B.params, ctx, panel, IDB_PARAM_BASE, IDB_VSCROLL) != 0)
        return -1;

    B.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS |
                             LVS_NOSORTHEADER | WS_TABSTOP,
                             0, 0, 100, 100, panel, (HMENU)(INT_PTR)IDB_LIST,
                             ctx->inst, NULL);
    if (B.list) {
        ListView_SetExtendedListViewStyle(B.list,
                                          LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES |
                                          LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        batch_list_col(0, L"文件名称", ctx->ds(300));
        batch_list_col(1, L"大小", ctx->ds(88));
        batch_list_col(2, L"类型", ctx->ds(64));
        batch_list_col(3, L"图片数", ctx->ds(70));
        batch_list_col(4, L"用时", ctx->ds(84));
        batch_list_col(5, L"状态", ctx->ds(460));
    }

    B.meta = ctx->mk(panel, L"STATIC", L"请选择源文件夹。", SS_LEFTNOWORDWRAP, 0,
                     IDB_META, ctx->font_ui);

    B.lbl_out = ctx->mk(panel, L"STATIC", L"保存到:", SS_LEFT, 0, 0, ctx->font_ui);
    B.outdir = ctx->mk(panel, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP,
                       WS_EX_CLIENTEDGE, IDB_OUTDIR, ctx->font_ui);
    B.browse = ctx->mk(panel, L"BUTTON", L"浏览…", BS_PUSHBUTTON | WS_TABSTOP, 0,
                       IDB_BROWSE, ctx->font_ui);
    B.save_img = ctx->mk(panel, L"BUTTON", L"保存图片", BS_PUSHBUTTON | WS_TABSTOP, 0,
                         IDB_SAVE_IMG, ctx->font_ui);
    B.save_txt = ctx->mk(panel, L"BUTTON", L"保存为 TXT…", BS_PUSHBUTTON | WS_TABSTOP, 0,
                         IDB_SAVE_TXT, ctx->font_ui);
    B.redact = ctx->mk(panel, L"BUTTON", L"替换 base64",
                       BS_AUTOCHECKBOX | WS_TABSTOP, 0, IDB_REDACT, ctx->font_ui);
    if (B.redact)
        SendMessageW(B.redact, BM_SETCHECK, BST_UNCHECKED, 0);

    return 0;
}

void batchtest_layout(int px, int py, int pw, int ph)
{
    int m, gap, rh, x0, iw, y, y0, y1;
    int lblW, mtdW, sndW, stpW, clrW, pickW, selW, selNW, rhCombo;
    int hostTop, hostH, hostW, listTop, listH, listBottom;

    if (!B.built || !B.url)
        return;

    m = A.ctx.ds(12);
    gap = A.ctx.ds(8);
    rh = A.ctx.line_h();
    x0 = px + m;
    iw = pw - 2 * m;
    y = py + m;

    lblW = A.ctx.ds(64);
    mtdW = A.ctx.ds(90);
    sndW = A.ctx.ds(96);
    stpW = A.ctx.ds(64);
    clrW = A.ctx.ds(64);
    pickW = A.ctx.ds(104);
    selW = A.ctx.ds(56);
    selNW = A.ctx.ds(68);
    rhCombo = rh + 160;

    /* 行 1：地址 + 方法 + 开始 + 停止 + 清空 */
    MoveWindow(B.lbl_url, x0, y + A.ctx.ds(3), lblW, rh, TRUE);
    MoveWindow(B.url, x0 + lblW, y,
               iw - lblW - mtdW - sndW - stpW - clrW - gap * 4, rh, TRUE);
    MoveWindow(B.method, x0 + iw - mtdW - sndW - stpW - clrW - gap * 3, y,
               mtdW, rhCombo, TRUE);
    MoveWindow(B.send, x0 + iw - sndW - stpW - clrW - gap * 2, y, sndW, rh, TRUE);
    MoveWindow(B.stop, x0 + iw - stpW - clrW - gap, y, stpW, rh, TRUE);
    MoveWindow(B.clear, x0 + iw - clrW, y, clrW, rh, TRUE);

    /* 行 2：源文件夹 + 选择 + fileType */
    y += rh + gap;
    MoveWindow(B.lbl_dir, x0, y + A.ctx.ds(3), lblW, rh, TRUE);
    MoveWindow(B.dir_edit, x0 + lblW, y,
               iw - lblW - pickW - selW - selNW - gap * 3, rh, TRUE);
    MoveWindow(B.pick_dir, x0 + iw - pickW - selW - selNW - gap * 2, y, pickW, rh, TRUE);
    MoveWindow(B.sel_all, x0 + iw - selW - selNW - gap, y, selW, rh, TRUE);
    MoveWindow(B.sel_none, x0 + iw - selNW, y, selNW, rh, TRUE);

    /* 行 3：代理 + 超时 + 提示 */
    y += rh + gap;
    MoveWindow(B.proxy, x0, y + A.ctx.ds(2), A.ctx.ds(120), rh - 2, TRUE);
    MoveWindow(B.timeout, x0 + A.ctx.ds(126), y, A.ctx.ds(60), rh, TRUE);
    MoveWindow(B.lbl_sec, x0 + A.ctx.ds(190), y + A.ctx.ds(3), A.ctx.ds(56), rh, TRUE);
    MoveWindow(B.hint, x0 + A.ctx.ds(250), y + A.ctx.ds(3),
               iw - A.ctx.ds(250), rh, TRUE);

    /* 底部：按钮行 + 保存目录行 */
    y0 = py + ph - m - rh;
    y1 = y0 - gap - rh;
    listBottom = y1 - gap - rh - A.ctx.ds(2);   /* 表格下方还要留状态行 */

    /* 参数区（可滚动，固定高度） */
    hostTop = y + rh + gap;
    hostH = A.ctx.ds(196);
    if (hostTop + hostH + A.ctx.ds(120) > listBottom - gap)
        hostH = listBottom - gap - hostTop - A.ctx.ds(120);
    if (hostH < A.ctx.ds(80))
        hostH = A.ctx.ds(80);
    hostW = iw - A.ctx.ds(20);

    MoveWindow(B.params.vsb, x0 + iw - A.ctx.ds(16), hostTop, A.ctx.ds(16), hostH, TRUE);
    MoveWindow(B.params.host, x0, hostTop, hostW, hostH, TRUE);
    B.params.view_h = hostH;
    params_layout_content(&B.params, hostW);
    params_update_scrollbar(&B.params);
    params_apply_scroll(&B.params);

    /* 表格 + 状态行 */
    listTop = hostTop + hostH + gap;
    listH = listBottom - listTop;
    if (listH < A.ctx.ds(60))
        listH = A.ctx.ds(60);
    if (B.list)
        MoveWindow(B.list, x0, listTop, iw, listH, TRUE);

    /* 状态行紧贴表格下方 */
    MoveWindow(B.meta, x0, listTop + listH + A.ctx.ds(2), iw, rh, TRUE);

    MoveWindow(B.lbl_out, x0, y1 + A.ctx.ds(3), A.ctx.ds(56), rh, TRUE);
    MoveWindow(B.outdir, x0 + A.ctx.ds(56), y1,
               iw - A.ctx.ds(56) - A.ctx.ds(80) - gap, rh, TRUE);
    MoveWindow(B.browse, x0 + iw - A.ctx.ds(80), y1, A.ctx.ds(80), rh, TRUE);

    {
        int x = x0;
        int w1 = A.ctx.ds(96), w2 = A.ctx.ds(96), w3 = A.ctx.ds(120), w4 = A.ctx.ds(110);

        MoveWindow(B.save_img, x, y0, w1, rh, TRUE); x += w1 + gap;
        MoveWindow(B.save_txt, x, y0, w2, rh, TRUE); x += w2 + gap;
        MoveWindow(B.redact, x, y0 + A.ctx.ds(2), w3, rh - 2, TRUE);
        (void)w4;
    }
}

/* ---------- 保存 ---------- */

static void batch_timestamp(wchar_t *out, size_t cch)
{
    SYSTEMTIME st;

    GetLocalTime(&st);
    wset(out, cch, L"%04d%02d%02d_%02d%02d%02d",
         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

static void batch_save_images(void)
{
    wchar_t dir[1024], ts[32], err[512];
    int i, sel, ok = 0, bad = 0;
    unsigned long long total = 0;

    if (B.running) {
        msg_error(L"批量请求还在进行中，完成后再保存。");
        return;
    }
    if (B.count == 0) {
        msg_error(L"还没有文件。先选择源文件夹。");
        return;
    }
    sel = batch_selected_count();
    if (sel == 0) {
        msg_error(L"请先在表格里勾选要保存的文件（左侧复选框）。");
        return;
    }

    /*
     * 目录已经在底部"保存到:"里选过了，这里不再弹一次。
     * 只有它真的为空（理论上 init_state 已填好）才让人选。
     */
    wset(dir, 1024, L"%s", B.outdir_path);
    if (!dir[0] &&
        (!A.ctx.pick_folder ||
         !A.ctx.pick_folder(main_window(), L"选择图片保存文件夹", dir, 1024)))
        return;

    batch_timestamp(ts, 32);

    for (i = 0; i < B.count; i++) {
        batch_row *row = &B.rows[i];
        wchar_t stem[64], path[1024], wext[16];
        size_t k;
        int seq = 0;

        if (!batch_list_checked(i))
            continue;
        if (!row->res.net_ok || row->res.images.count == 0)
            continue;

        batch_stem(row->name, stem, 64);

        for (k = 0; k < row->res.images.count; k++) {
            const jl_image_ref *ref = &row->res.images.items[k];
            const char *payload;
            size_t plen = 0, cap, dn;
            unsigned char *buf;

            if (strstr(ref->path, "inputImage") != NULL)
                continue;

            payload = jl_image_payload_ptr(row->res.http.data, ref, &plen);
            if (!payload || plen == 0) {
                bad++;
                continue;
            }
            cap = b64_decoded_cap(plen);
            buf = (unsigned char *)malloc(cap ? cap : 1);
            if (!buf) {
                bad++;
                continue;
            }
            dn = b64_decode(payload, plen, buf, cap);
            if (dn == (size_t)-1 || dn == 0) {
                free(buf);
                bad++;
                continue;
            }

            seq++;
            ascii_to_wide((ref->ext[0] == '.') ? ref->ext : ".bin", wext, 16);
            /* 文件名：源文件主名 + 时间戳 + 该响应内图片序号 */
            wset(path, 1024, L"%s\\%s_%s_%02d%s", dir, stem, ts, seq, wext);
            if (A.ctx.unique_path)
                A.ctx.unique_path(path, 1024);

            if (A.ctx.write_file && A.ctx.write_file(path, buf, dn, err, 512)) {
                ok++;
                total += dn;
            } else {
                bad++;
            }
            free(buf);
        }
    }

    if (ok == 0) {
        msg_error(L"没有保存到任何图片。\n\n"
                  L"常见原因：勾选的文件还没发送成功，或响应里只有 inputImage。");
        return;
    }
    {
        wchar_t sz[64];
        size_text(total, sz, sizeof(sz) / sizeof(sz[0]));
        wset(B.outdir_path, 1024, L"%s", dir);
        SetWindowTextW(B.outdir, B.outdir_path);
        msg_info(L"已保存 %d 张，失败 %d 张，共 %s。\n\n目录：\n%s",
                 ok, bad, sz, dir);
    }
}

static void batch_save_txt(void)
{
    wchar_t dir[1024], ts[32], err[512];
    int i, ok = 0, bad = 0, redact;

    if (B.running) {
        msg_error(L"批量请求还在进行中，完成后再保存。");
        return;
    }
    if (B.count == 0) {
        msg_error(L"还没有文件。先选择源文件夹。");
        return;
    }
    if (batch_selected_count() == 0) {
        msg_error(L"请先在表格里勾选要保存的文件（左侧复选框）。");
        return;
    }
    wset(dir, 1024, L"%s", B.outdir_path);
    if (!dir[0] &&
        (!A.ctx.pick_folder ||
         !A.ctx.pick_folder(main_window(), L"选择文本保存文件夹", dir, 1024)))
        return;

    redact = redact_on(B.redact) ? 1 : 0;
    batch_timestamp(ts, 32);

    for (i = 0; i < B.count; i++) {
        batch_row *row = &B.rows[i];
        wchar_t stem[64], path[1024];
        char *out = NULL;
        size_t outLen = 0;

        if (!batch_list_checked(i))
            continue;
        if (!row->res.net_ok || !row->res.http.data)
            continue;

        if (redact) {
            if (make_redacted(&row->res, &out, &outLen) != 0)
                continue;
        } else {
            out = row->res.http.data;
            outLen = row->res.http.len;
        }

        batch_stem(row->name, stem, 64);
        /* 文件名：源文件主名 + 时间戳 */
        wset(path, 1024, L"%s\\%s_%s.txt", dir, stem, ts);
        if (A.ctx.unique_path)
            A.ctx.unique_path(path, 1024);

        if (A.ctx.write_file && A.ctx.write_file(path, out, outLen, err, 512))
            ok++;
        else
            bad++;

        if (redact)
            free(out);
    }

    if (ok == 0) {
        msg_error(L"没有保存到任何文本。\n\n勾选的文件可能都还没成功返回。");
        return;
    }
    wset(B.outdir_path, 1024, L"%s", dir);
    SetWindowTextW(B.outdir, B.outdir_path);
    msg_info(L"已保存 %d 个文本文件，失败 %d 个%s。\n\n目录：\n%s",
             ok, bad, redact ? L"（base64 已替换为 \"...\"）" : L"（完整响应）", dir);
}

/* ---------- 线程 ---------- */

typedef struct {
    wchar_t        url[2048];
    int            method_get, use_proxy, file_type;
    DWORD          timeout_ms;
    api_param_state params[API_MAX_PARAMS];
    HWND           notify;
    UINT_PTR       gen;
} batch_job;

static DWORD WINAPI batch_worker(LPVOID param)
{
    batch_job *job = (batch_job *)param;
    int i;

    for (i = 0; i < B.count; i++) {
        api_result *res;
        api_job j;

        if (InterlockedCompareExchange(&B.stop_flag, 1, 1) == 1)
            break;

        res = (api_result *)calloc(1, sizeof(*res));
        if (!res)
            break;

        memset(&j, 0, sizeof(j));
        wset(j.path, 1024, L"%s", B.rows[i].path);
        wset(j.url, 2048, L"%s", job->url);
        j.method_get = job->method_get;
        j.use_proxy = job->use_proxy;
        j.file_type = job->file_type;
        j.timeout_ms = job->timeout_ms;
        memcpy(j.params, job->params, sizeof(j.params));

        api_run(&j, res);
        PostMessageW(job->notify, WM_BATCH_ROW, (WPARAM)i, (LPARAM)res);
    }

    PostMessageW(job->notify, WM_BATCH_DONE, 0, 0);
    free(job);
    return 0;
}

static void batch_set_busy(int busy)
{
    EnableWindow(B.send, busy ? FALSE : TRUE);
    EnableWindow(B.stop, busy ? TRUE : FALSE);
    EnableWindow(B.pick_dir, busy ? FALSE : TRUE);
    EnableWindow(B.clear, busy ? FALSE : TRUE);
    EnableWindow(B.url, busy ? FALSE : TRUE);
    EnableWindow(B.method, busy ? FALSE : TRUE);
    EnableWindow(B.sel_all, busy ? FALSE : TRUE);
    EnableWindow(B.sel_none, busy ? FALSE : TRUE);
    EnableWindow(B.proxy, busy ? FALSE : TRUE);
    EnableWindow(B.timeout, busy ? FALSE : TRUE);
    EnableWindow(B.save_img, busy ? FALSE : TRUE);
    EnableWindow(B.save_txt, busy ? FALSE : TRUE);
    params_set_enabled(&B.params, busy ? 0 : 1);
}

static void batch_start(void)
{
    batch_job *job;
    HANDLE th;
    int i;
    wchar_t url[2048];

    if (B.running)
        return;
    if (B.count == 0) {
        msg_error(L"还没有文件。先点「选择文件夹…」挑一个装 PDF / 图片的目录。");
        return;
    }

    url[0] = L'\0';
    {
        wchar_t *t = get_text(B.url);
        wset(url, 2048, L"%s", t ? t : L"");
        free(t);
    }
    if (!url[0]) {
        msg_error(L"请填写接口地址。");
        return;
    }

    job = (batch_job *)calloc(1, sizeof(*job));
    if (!job) {
        msg_error(L"内存不足。");
        return;
    }

    /* 清掉上一轮的响应，避免内存一直累积 */
    for (i = 0; i < B.count; i++) {
        result_free(&B.rows[i].res);
        memset(&B.rows[i].res, 0, sizeof(api_result));
        B.rows[i].done = 0;
        B.rows[i].images = 0;
        B.rows[i].trimmed = 0;
        batch_list_reset_row(i);      /* 只清显示，保留用户已勾选的行 */
    }
    B_resp_bytes = 0;

    wset(job->url, 2048, L"%s", url);
    job->method_get = ((int)SendMessageW(B.method, CB_GETCURSEL, 0, 0) == 1) ? 1 : 0;
    job->file_type = -1;      /* -1 = 每个文件按内容（魔数）自动判断 PDF / 图片 */
    job->use_proxy = (SendMessageW(B.proxy, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    {
        wchar_t *t = get_text(B.timeout);
        int sec = t ? (int)wcstol(t, NULL, 10) : 0;

        free(t);
        if (sec <= 0)
            sec = 10;
        if (sec > 3600)
            sec = 3600;
        job->timeout_ms = (DWORD)sec * 1000u;
    }
    params_collect(&B.params, job->params);
    job->notify = main_window();
    B.gen++;
    job->gen = B.gen;

    B.stop_flag = 0;
    B.running = 1;
    batch_set_busy(1);
    set_static(B.meta, L"开始：共 %d 个文件，逐个发送…", B.count);
    set_static(B.hint, L"批量进行中：参数已锁定，可在表格里看到每个文件的进度");

    th = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)batch_worker, job, 0, NULL);
    if (!th) {
        B.running = 0;
        batch_set_busy(0);
        free(job);
        msg_error(L"无法创建线程，批量未开始。");
        return;
    }
    CloseHandle(th);
}

void batchtest_on_row(int idx, api_result *res)
{
    batch_row *row;
    size_t k;

    if (!res)
        return;
    if (idx < 0 || idx >= B.count) {
        free(res);
        return;
    }
    row = &B.rows[idx];
    result_free(&row->res);
    row->res = *res;
    free(res);

    row->done = 1;
    row->images = 0;
    for (k = 0; k < row->res.images.count; k++) {
        if (strstr(row->res.images.items[k].path, "inputImage") == NULL)
            row->images++;
    }

    /*
     * 内存预算：超了就把响应体放掉（图片列表引用的正是这块内存，必须一起清）。
     * 表格里的名称/大小/图片数/状态不受影响，只是不能再从这一行导出图片或文本。
     */
    B_resp_bytes += row->res.http.len;
    if (B_resp_bytes > BATCH_RESP_BUDGET && row->res.http.data) {
        hc_response_free(&row->res.http);
        jl_image_list_free(&row->res.images);
        memset(&row->res.images, 0, sizeof(row->res.images));
        row->trimmed = 1;
    }
    batch_list_update_row(idx);

    {
        int n = 0, i;
        for (i = 0; i < B.count; i++)
            if (B.rows[i].done)
                n++;
        set_static(B.meta, L"进行中：%d / %d 个文件已返回（当前 %s）",
             n, B.count, B.rows[idx].name);
    }
}

void batchtest_on_finished(void)
{
    int i, ok = 0, bad = 0, imgs = 0;

    for (i = 0; i < B.count; i++) {
        if (!B.rows[i].done)
            continue;
        if (B.rows[i].res.net_ok && !(B.rows[i].res.has_biz && B.rows[i].res.biz_code != 0))
            ok++;
        else
            bad++;
        imgs += B.rows[i].images;
    }
    B.running = 0;
    batch_set_busy(0);

    if (B.stop_flag)
        set_static(B.meta, L"已停止：成功 %d，失败 %d，可保存图片 %d 张（未处理的文件保持“待发送”）",
             ok, bad, imgs);
    else
        set_static(B.meta, L"完成：成功 %d，失败 %d，可保存图片 %d 张。勾选需要的文件后点“保存图片 / 保存为 TXT…”",
             ok, bad, imgs);
    set_static(B.hint, L"批量结束。保存时只会处理表格里勾选的行。");
}

void batchtest_stop(void)
{
    if (!B.running)
        return;
    InterlockedExchange(&B.stop_flag, 1);
    set_static(B.meta, L"正在停止：等当前文件返回后结束（WinHTTP 不支持中断进行中的请求）…");
}

int batchtest_is_busy(void)
{
    return B.running ? 1 : 0;
}

void batchtest_on_close(void)
{
    InterlockedExchange(&B.stop_flag, 1);
    batch_clear_rows();
    B.running = 0;
}

/* ---------- 消息 ---------- */

void batchtest_pick_dir(void)
{
    wchar_t dir[1024], err[512];
    wchar_t *cur;

    cur = get_text(B.dir_edit);
    wset(dir, 1024, L"%s", (cur && cur[0]) ? cur : B.outdir_path);
    free(cur);

    if (!A.ctx.pick_folder ||
        !A.ctx.pick_folder(main_window(), L"选择源文件夹（PDF / 图片）", dir, 1024))
        return;

    if (batch_collect(dir, err, 512) <= 0) {
        msg_error(L"%s", err[0] ? err : L"这个文件夹里没有可用文件。");
        return;
    }
    SetWindowTextW(B.dir_edit, dir);
    batch_list_fill();
    {
        unsigned long long total = 0;
        wchar_t sz[64];
        int i;

        for (i = 0; i < B.count; i++)
            total += B.rows[i].size;
        size_text(total, sz, sizeof(sz) / sizeof(sz[0]));
        set_static(B.meta, L"已载入 %d 个文件，合计 %s。点“开始批量”发送全部文件。",
                   B.count, sz);
    }
    set_static(B.hint, L"勾选只决定“保存哪些”；请求始终发送全部文件（类型按内容自动判断）");
}

int batchtest_on_command(HWND hMain, WORD id, WORD code)
{
    (void)hMain;

    if (params_on_command(&B.params, (int)id, (int)code))
        return 1;

    switch (id) {
    case IDB_PICK_DIR:
        if (code == BN_CLICKED)
            batchtest_pick_dir();
        return 1;

    case IDB_SEND:
        if (code == BN_CLICKED)
            batch_start();
        return 1;

    case IDB_STOP:
        if (code == BN_CLICKED)
            batchtest_stop();
        return 1;

    case IDB_CLEAR:
        if (code == BN_CLICKED) {
            if (B.running) {
                msg_error(L"批量还在进行中，先点“停止”。");
                return 1;
            }
            batch_clear_rows();
            SetWindowTextW(B.dir_edit, L"");
            set_static(B.meta, L"请选择源文件夹。");
            set_static(B.hint, L"");
        }
        return 1;

    case IDB_BROWSE:
        if (code == BN_CLICKED) {
            wchar_t dir[1024];

            wset(dir, 1024, L"%s", B.outdir_path);
            if (A.ctx.pick_folder &&
                A.ctx.pick_folder(main_window(), L"选择保存文件夹", dir, 1024)) {
                wset(B.outdir_path, 1024, L"%s", dir);
                SetWindowTextW(B.outdir, B.outdir_path);
            }
        }
        return 1;

    case IDB_SEL_ALL:
    case IDB_SEL_NONE:
        if (code == BN_CLICKED && B.list && !B.running) {
            int i;
            UINT state = (id == IDB_SEL_ALL) ? INDEXTOSTATEIMAGEMASK(2)
                                             : INDEXTOSTATEIMAGEMASK(1);

            for (i = 0; i < B.count; i++)
                ListView_SetItemState(B.list, i, state, LVIS_STATEIMAGEMASK);
            set_static(B.meta, (id == IDB_SEL_ALL)
                                   ? L"已全选 %d 个文件；保存时只处理勾选的行。"
                                   : L"已取消全部勾选。",
                       B.count);
        }
        return 1;

    case IDB_SAVE_IMG:
        if (code == BN_CLICKED)
            batch_save_images();
        return 1;

    case IDB_SAVE_TXT:
        if (code == BN_CLICKED)
            batch_save_txt();
        return 1;

    default:
        break;
    }
    return 0;
}

int batchtest_on_notify(NMHDR *nm)
{
    if (!nm || !B.list || nm->hwndFrom != B.list)
        return 0;
    /* 勾选变化不需要额外处理：保存时直接读 TableView 的状态 */
    return 1;
}

void batchtest_on_vscroll(WPARAM wp)
{
    params_vscroll(&B.params, wp);
}

void batchtest_on_wheel(int delta)
{
    params_wheel(&B.params, delta);
}

int batchtest_wants_wheel(void)
{
    return params_hit(&B.params);
}

void batchtest_init_state(void)
{
    wchar_t dir[1024];

    if (!B.built)
        return;

    params_defaults(&B.params);
    if (B.redact)
        SendMessageW(B.redact, BM_SETCHECK, BST_UNCHECKED, 0);

    /* 默认保存目录：EXE 所在目录（与第 4 页一致，不写注册表） */
    GetModuleFileNameW(NULL, dir, 1024);
    {
        wchar_t *s = wcsrchr(dir, L'\\');
        if (s)
            *s = L'\0';
    }
    wset(B.outdir_path, 1024, L"%s", dir);
    SetWindowTextW(B.outdir, B.outdir_path);
    set_static(B.meta, L"请选择源文件夹。");
    set_static(B.hint, L"");
}

int batchtest_ui_ok(void)
{
    return (B.built && B.url && B.send && B.list && B.meta &&
            B.pick_dir && B.save_img && B.params.host && B.params.vsb) ? 1 : 0;
}

void batchtest_diag(wchar_t *buf, size_t cch)
{
    wchar_t line[512];
    RECT r;
    size_t used;

    if (!B.built || !buf || cch < 64)
        return;
    used = wcslen(buf);
    if (used >= cch - 1)
        return;

    wcsncat(buf, L"\r\n--- 第 5 页（批量）---\r\n", cch - used - 1);
    used = wcslen(buf);
    if (used >= cch - 1)
        return;
    if (B.list) {
        GetWindowRect(B.list, &r);
        wset(line, 512, L"表格 %dx%d   文件数=%d   已勾选=%d\r\n",
             (int)(r.right - r.left), (int)(r.bottom - r.top),
             B.count, batch_selected_count());
        wcsncat(buf, line, cch - wcslen(buf) - 1);
    }
    if (B.params.host) {
        GetWindowRect(B.params.host, &r);
        wset(line, 512, L"参数宿主 %dx%d   内容高=%d   可视高=%d\r\n",
             (int)(r.right - r.left), (int)(r.bottom - r.top),
             B.params.content_h, B.params.view_h);
        wcsncat(buf, line, cch - wcslen(buf) - 1);
    }
}
