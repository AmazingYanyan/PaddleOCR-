/*
 * httpclient.h - 极薄的 WinHTTP 封装
 *
 * 只做一件事：把一段 JSON 打到指定 URL，把响应原样收回来，并把失败原因
 * 翻译成能看懂的中文。不做重试、不做连接复用、不做异步——请求由界面线程
 * 派给一个 worker 线程同步执行（见 apitest.c）。
 *
 * 依赖 winhttp.dll，这是 Windows 自带组件（XP SP2 起），不需要额外分发。
 */
#ifndef HTTPCLIENT_H
#define HTTPCLIENT_H

#include <windows.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 上限：避免一次请求把进程内存打爆（网关返回几十 MB 图时尤其重要） */
#ifdef _WIN64
#define HC_MAX_RESPONSE   ((size_t)128 * 1024 * 1024)
#else
#define HC_MAX_RESPONSE   ((size_t)64 * 1024 * 1024)
#endif
#define HC_MAX_BODY       ((size_t)48 * 1024 * 1024)

typedef struct {
    wchar_t    url[2048];        /* 可省略 http://，内部会补 */
    int        method_get;       /* 0 = POST，1 = GET（GET 不带 body） */
    const char *body;            /* UTF-8 请求体，可 NULL */
    size_t     body_len;
    int        use_system_proxy; /* 0 = 直连（默认），1 = 用系统代理设置 */
    DWORD      timeout_ms;       /* 接收超时；0 时用默认 180 秒 */
} hc_request;

typedef struct {
    int      http_status;        /* 0 表示没拿到响应（连接失败等） */
    wchar_t  status_text[128];
    wchar_t  content_type[128];
    long long content_length;    /* -1 表示未知 */

    char    *data;               /* 响应体（以 '\0' 结尾），需 hc_response_free */
    size_t   len;

    DWORD    win32_err;          /* 网络层错误码，0 表示网络层正常 */
    wchar_t  stage[32];          /* 出错阶段：解析URL / 连接 / 发送 / 等待响应 / 读取响应 */
    wchar_t  err_text[512];      /* 中文错误说明 */

    unsigned long long elapsed_ms;   /* 整个请求耗时 */
} hc_response;

/*
 * 执行一次请求。返回 0 表示网络层成功（注意：HTTP 4xx/5xx 也算网络层成功，
 * 需要调用方自己看 http_status）。返回 -1 表示连接/发送/读取失败，
 * 详情在 resp->stage 与 resp->err_text。
 */
int  hc_execute(const hc_request *req, hc_response *resp);
void hc_response_free(hc_response *r);

/* 把 Windows / WinHTTP 错误码翻译成中文（自检与界面都会用到） */
void hc_error_text(DWORD code, wchar_t *out, size_t cch);

#ifdef __cplusplus
}
#endif

#endif /* HTTPCLIENT_H */
