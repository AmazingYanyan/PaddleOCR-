/*
 * httpclient.c - WinHTTP 封装
 *
 * 几个刻意的取舍：
 *   1) 默认 NO_PROXY（直连）。内网接口走系统代理/PAC 时，WinHTTP 的自动探测
 *      可能先卡 5~20 秒；界面上给了「使用系统代理」勾选，需要时再打开。
 *   2) 大 body（几十 MB 的 PDF Base64）一次性发送失败时，回退为
 *      "先发头 + WinHttpWriteData 分块写"。部分环境会在一次发送大 body 时
 *      返回 122（缓冲区不足）或 8（内存不足）。
 *   3) 响应流式读取：WinHttpQueryDataAvailable + 循环 WinHttpReadData，
 *      单次 ReadData 可能读不满，必须循环到 avail 归零。
 *   4) 所有 DWORD 出参一律用真正的 DWORD 变量接。这是上一版 ReadFile
 *      崩溃事故的同类风险，绝不能再犯。
 */
#include "httpclient.h"

#include <winhttp.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ------------------------------------------------------------------ */

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

typedef struct {
    DWORD code;
    const wchar_t *text;
} err_map;

static const err_map ERRORS[] = {
    { 12001, L"缓冲区不足（响应头过大）" },
    { 12002, L"超时：在限定时间内服务器没有响应。检查服务是否卡住、网络是否可达，必要时调整超时时间" },
    { 12003, L"无法解析服务器地址" },
    { 12004, L"URL 无效" },
    { 12005, L"URL 无效：缺少主机名或格式不对" },
    { 12006, L"不支持的协议：本工具只支持 http:// 与 https://" },
    { 12007, L"域名解析失败。内网环境建议直接填 IP 地址" },
    { 12009, L"URL 的协议与端口不匹配" },
    { 12011, L"URL 包含非法字符" },
    { 12012, L"无法打开网络会话" },
    { 12015, L"重定向次数过多" },
    { 12016, L"操作已终止" },
    { 12017, L"操作被取消" },
    { 12018, L"网络句柄已关闭" },
    { 12019, L"网络句柄状态不允许该操作" },
    { 12029, L"无法连接到服务器。请检查 IP/端口是否正确、服务是否已启动、防火墙是否放行" },
    { 12030, L"连接被服务器重置" },
    { 12031, L"连接已断开" },
    { 12032, L"请求被服务器拒绝" },
    { 12037, L"服务器证书已过期" },
    { 12038, L"服务器证书与主机名不匹配" },
    { 12044, L"服务器证书链不完整或不受信任" },
    { 12152, L"服务器返回了无法识别的 HTTP 响应（这个端口可能不是 HTTP 服务）" },
    { 12175, L"HTTPS 证书校验失败（自签证书请改用 http，或把根证书导入系统）" },
    { 122,   L"缓冲区不足（请求体过大）" },
    { 8,     L"内存不足" },
    { 87,    L"参数错误" },
    { 5,     L"拒绝访问" },
    { 0,     NULL }
};

void hc_error_text(DWORD code, wchar_t *out, size_t cch)
{
    int i;
    wchar_t sys[384];

    if (!out || cch == 0)
        return;

    for (i = 0; ERRORS[i].text; i++) {
        if (ERRORS[i].code == code) {
            wcsncpy(out, ERRORS[i].text, cch - 1);
            out[cch - 1] = L'\0';
            return;
        }
    }

    sys[0] = L'\0';
    if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, code, 0, sys, (DWORD)(sizeof(sys) / sizeof(sys[0])),
                       NULL) > 0) {
        /* 去掉系统消息尾部的换行 */
        size_t n = wcslen(sys);
        while (n > 0 && (sys[n - 1] == L'\r' || sys[n - 1] == L'\n' || sys[n - 1] == L' '))
            sys[--n] = L'\0';
        wset(out, cch, L"错误码 %lu：%s", (unsigned long)code, sys);
    } else {
        wset(out, cch, L"错误码 %lu", (unsigned long)code);
    }
}

/* 规范化 URL：允许用户只填 10.0.0.1:8080/path */
static int normalize_url(const wchar_t *in, wchar_t *out, size_t cch)
{
    const wchar_t *p = in;
    size_t n;

    if (!in || !out || cch == 0)
        return 0;
    while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n')
        p++;
    if (!*p)
        return 0;

    if (wcsstr(p, L"://"))
        wset(out, cch, L"%s", p);
    else
        wset(out, cch, L"http://%s", p);

    /* 去掉尾部空白 */
    n = wcslen(out);
    while (n > 0 && (out[n - 1] == L' ' || out[n - 1] == L'\t' ||
                     out[n - 1] == L'\r' || out[n - 1] == L'\n'))
        out[--n] = L'\0';
    return n > 0;
}

static void resp_init(hc_response *r)
{
    memset(r, 0, sizeof(*r));
    r->content_length = -1;
}

void hc_response_free(hc_response *r)
{
    if (!r)
        return;
    free(r->data);
    r->data = NULL;
    r->len = 0;
}

static void set_stage(hc_response *r, const wchar_t *stage, DWORD err)
{
    wcsncpy(r->stage, stage, sizeof(r->stage) / sizeof(r->stage[0]) - 1);
    r->stage[sizeof(r->stage) / sizeof(r->stage[0]) - 1] = L'\0';
    r->win32_err = err;
    hc_error_text(err, r->err_text, sizeof(r->err_text) / sizeof(r->err_text[0]));
}

/* 追加式扩展响应缓冲；返回 0 表示超限或内存不足 */
static int grow(char **buf, size_t *cap, size_t need)
{
    char *nb;
    size_t ncap;

    if (need > HC_MAX_RESPONSE)
        return 0;
    if (need <= *cap)
        return 1;

    ncap = *cap ? *cap : (size_t)64 * 1024;
    while (ncap < need) {
        if (ncap > HC_MAX_RESPONSE / 2) {
            ncap = HC_MAX_RESPONSE;
            break;
        }
        ncap *= 2;
    }
    if (ncap < need)
        return 0;

    nb = (char *)realloc(*buf, ncap);
    if (!nb)
        return 0;
    *buf = nb;
    *cap = ncap;
    return 1;
}

/* ------------------------------------------------------------------ */

int hc_execute(const hc_request *req, hc_response *resp)
{
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;
    wchar_t url[2048];
    wchar_t host[512];
    wchar_t target[4096];
    URL_COMPONENTS uc;
    int secure = 0;
    int rc = -1;
    DWORD tmo;
    unsigned long long t0;

    if (!resp)
        return -1;
    resp_init(resp);
    t0 = GetTickCount64();

    if (!req || !req->url[0]) {
        wset(resp->stage, 32, L"参数");
        wset(resp->err_text, 512, L"目标 URL 为空");
        return -1;
    }
    if (req->body_len > HC_MAX_BODY) {
        wset(resp->stage, 32, L"参数");
        wset(resp->err_text, 512, L"请求体 %I64u 字节，超过上限 %I64u 字节",
             (unsigned long long)req->body_len, (unsigned long long)HC_MAX_BODY);
        return -1;
    }
    if (!normalize_url(req->url, url, sizeof(url) / sizeof(url[0]))) {
        wset(resp->stage, 32, L"解析URL");
        wset(resp->err_text, 512, L"URL 为空或全是空白");
        return -1;
    }

    /* ---- 解析 URL ---- */
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    uc.dwUserNameLength = (DWORD)-1;
    uc.dwPasswordLength = (DWORD)-1;

    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        set_stage(resp, L"解析URL", GetLastError());
        return -1;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS) {
        wset(resp->stage, 32, L"解析URL");
        wset(resp->err_text, 512, L"不支持的协议：只支持 http:// 与 https://");
        return -1;
    }
    secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    /*
     * 注意：WinHttpCrackUrl 返回的长度单位是"字符"（wchar_t 个数），
     * 不是字节。这里若按字节去算，路径会被砍掉一半。
     */
    if (uc.lpszHostName == NULL || uc.dwHostNameLength == 0 ||
        uc.dwHostNameLength >= (DWORD)(sizeof(host) / sizeof(host[0]))) {
        wset(resp->stage, 32, L"解析URL");
        wset(resp->err_text, 512, L"URL 里没有有效的主机名");
        return -1;
    }
    memcpy(host, uc.lpszHostName, (size_t)uc.dwHostNameLength * sizeof(wchar_t));
    host[uc.dwHostNameLength] = L'\0';

    /* 请求目标 = 路径 + 查询串（去掉 #fragment） */
    {
        size_t tl = 0;
        DWORD i;

        if (uc.dwUrlPathLength > 0 && uc.lpszUrlPath) {
            for (i = 0; i < uc.dwUrlPathLength && tl + 1 < 4096; i++)
                target[tl++] = uc.lpszUrlPath[i];
        }
        if (tl == 0)
            target[tl++] = L'/';
        if (uc.dwExtraInfoLength > 0 && uc.lpszExtraInfo) {
            for (i = 0; i < uc.dwExtraInfoLength && tl + 1 < 4096; i++) {
                if (uc.lpszExtraInfo[i] == L'#')
                    break;
                target[tl++] = uc.lpszExtraInfo[i];
            }
        }
        target[tl] = L'\0';
    }

    tmo = req->timeout_ms ? req->timeout_ms : 180000;

    /* ---- 会话 ---- */
    hSession = WinHttpOpen(L"B64ShaTool/2.0",
                           req->use_system_proxy ? WINHTTP_ACCESS_TYPE_DEFAULT_PROXY
                                                 : WINHTTP_ACCESS_TYPE_NO_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        set_stage(resp, L"打开会话", GetLastError());
        goto done;
    }
    WinHttpSetTimeouts(hSession, 5000, 10000, 120000, tmo);

    hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) {
        set_stage(resp, L"连接", GetLastError());
        goto done;
    }

    hRequest = WinHttpOpenRequest(hConnect,
                                  req->method_get ? L"GET" : L"POST",
                                  target, NULL,
                                  WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        set_stage(resp, L"创建请求", GetLastError());
        goto done;
    }

    /* Header 固定只有 Content-Type；不声明 Accept-Encoding，避免服务端回压缩内容 */
    WinHttpAddRequestHeaders(hRequest, L"Content-Type: application/json",
                             (DWORD)-1,
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    /* ---- 发送 ---- */
    if (req->method_get || !req->body || req->body_len == 0) {
        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                NULL, 0, 0, 0)) {
            set_stage(resp, L"发送", GetLastError());
            goto done;
        }
    } else {
        DWORD blen = (DWORD)req->body_len;
        BOOL ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     (LPVOID)req->body, blen, blen, 0);
        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_INSUFFICIENT_BUFFER || e == ERROR_NOT_ENOUGH_MEMORY) {
                /* 大 body 回退路径：先发头，再分块写 */
                size_t off = 0;
                if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                        NULL, 0, blen, 0)) {
                    set_stage(resp, L"发送", GetLastError());
                    goto done;
                }
                while (off < req->body_len) {
                    DWORD chunk, wrote = 0;
                    size_t left = req->body_len - off;
                    chunk = (DWORD)(left > 65536 ? 65536 : left);
                    if (!WinHttpWriteData(hRequest, req->body + off, chunk, &wrote)) {
                        set_stage(resp, L"发送", GetLastError());
                        goto done;
                    }
                    if (wrote == 0) {
                        wset(resp->stage, 32, L"发送");
                        wset(resp->err_text, 512,
                             L"分块写入请求体时没有任何字节被接受（已写入 %I64u / %I64u 字节）",
                             (unsigned long long)off, (unsigned long long)req->body_len);
                        goto done;
                    }
                    off += wrote;
                }
            } else {
                set_stage(resp, L"发送", e);
                goto done;
            }
        }
    }

    /* ---- 等响应 ---- */
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        set_stage(resp, L"等待响应", GetLastError());
        goto done;
    }

    /* ---- 状态码 / 头部 ---- */
    {
        DWORD code = 0;
        DWORD csz = sizeof(code);

        if (!WinHttpQueryHeaders(hRequest,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &code, &csz,
                                 WINHTTP_NO_HEADER_INDEX)) {
            set_stage(resp, L"读取状态码", GetLastError());
            goto done;
        }
        resp->http_status = (int)code;
    }
    {
        wchar_t text[128];
        DWORD tsz = (DWORD)sizeof(text);

        text[0] = L'\0';
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_TEXT,
                                WINHTTP_HEADER_NAME_BY_INDEX, text, &tsz,
                                WINHTTP_NO_HEADER_INDEX))
            wset(resp->status_text, 128, L"%s", text);
    }
    {
        wchar_t ct[128];
        DWORD csz = (DWORD)sizeof(ct);

        ct[0] = L'\0';
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_TYPE,
                                WINHTTP_HEADER_NAME_BY_INDEX, ct, &csz,
                                WINHTTP_NO_HEADER_INDEX))
            wset(resp->content_type, 128, L"%s", ct);
    }
    {
        DWORD clen = 0;
        DWORD csz = sizeof(clen);

        if (WinHttpQueryHeaders(hRequest,
                                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &clen, &csz,
                                WINHTTP_NO_HEADER_INDEX))
            resp->content_length = (long long)clen;
    }

    /* 服务端如果擅自压缩，这里明确报出来，而不是把二进制当 JSON 显示 */
    {
        wchar_t ce[64];
        DWORD csz = (DWORD)sizeof(ce);

        ce[0] = L'\0';
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_ENCODING,
                                WINHTTP_HEADER_NAME_BY_INDEX, ce, &csz,
                                WINHTTP_NO_HEADER_INDEX) && ce[0]) {
            if (_wcsicmp(ce, L"identity") != 0) {
                wset(resp->stage, 32, L"读取响应");
                wset(resp->err_text, 512,
                     L"服务器返回了压缩内容（Content-Encoding: %s），本工具不内置解压。"
                     L"请让服务端关闭压缩，或改用 Postman 查看原始响应", ce);
                goto done;
            }
        }
    }

    if (resp->content_length > (long long)HC_MAX_RESPONSE) {
        wset(resp->stage, 32, L"读取响应");
        wset(resp->err_text, 512,
             L"响应体 %I64d 字节，超过本工具上限 %I64u 字节（可在「可视化」等参数上关闭大图返回）",
             resp->content_length, (unsigned long long)HC_MAX_RESPONSE);
        goto done;
    }

    /* ---- 流式读取 ---- */
    {
        char *buf = NULL;
        size_t cap = 0, total = 0;

        for (;;) {
            DWORD avail = 0;

            if (!WinHttpQueryDataAvailable(hRequest, &avail)) {
                set_stage(resp, L"读取响应", GetLastError());
                free(buf);
                goto done;
            }
            if (avail == 0)
                break;                       /* 正常结束 */
            if (!grow(&buf, &cap, total + avail + 1)) {
                wset(resp->stage, 32, L"读取响应");
                wset(resp->err_text, 512,
                     L"响应体超过上限 %I64u 字节，已中止读取",
                     (unsigned long long)HC_MAX_RESPONSE);
                free(buf);
                goto done;
            }
            while (avail > 0) {
                DWORD want = avail > 65536 ? 65536 : avail;
                DWORD got = 0;

                if (!WinHttpReadData(hRequest, buf + total, want, &got)) {
                    set_stage(resp, L"读取响应", GetLastError());
                    free(buf);
                    goto done;
                }
                if (got == 0)
                    break;
                total += got;
                avail -= got;
            }
        }

        if (!buf) {
            buf = (char *)malloc(1);
            if (!buf) {
                wset(resp->stage, 32, L"读取响应");
                wset(resp->err_text, 512, L"内存不足，无法保存响应");
                goto done;
            }
        }
        buf[total] = '\0';
        resp->data = buf;
        resp->len = total;
    }

    rc = 0;

done:
    if (hRequest)
        WinHttpCloseHandle(hRequest);
    if (hConnect)
        WinHttpCloseHandle(hConnect);
    if (hSession)
        WinHttpCloseHandle(hSession);

    resp->elapsed_ms = GetTickCount64() - t0;
    return rc;
}
