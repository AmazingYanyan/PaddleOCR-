/*
 * selftest.c - 可执行文件内置自检
 *
 * 目的：在没有开发环境的离线 Windows 机器上，也能当场验证
 *       「这个 exe 里的算法在这台机器上确实算得对」。
 * 用法：B64ShaTool.exe /selftest   （也可写 --selftest 或 -selftest）
 * 覆盖：Base64 标准向量 / 往返一致性 / 大缓冲区、SHA-256 官方向量 / 补位边界 /
 *       百万字节流式处理、文件类型识别、以及本机运行期环境信息。
 */
#include "selftest.h"
#include "b64sha.h"
#include "jsonlite.h"
#include "apibody.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_pass, g_fail;

static void rep(char *out, size_t cch, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (*used >= cch - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(out + *used, cch - *used, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    *used += (size_t)n;
    if (*used >= cch)
        *used = cch - 1;
    out[*used] = '\0';
}

static void ok(char *out, size_t cch, size_t *used, int cond, const char *what)
{
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        rep(out, cch, used, "  [失败] %s\n", what);
    }
}

static void eq(char *out, size_t cch, size_t *used, const char *got,
               const char *want, const char *what)
{
    if (strcmp(got, want) == 0) {
        g_pass++;
    } else {
        g_fail++;
        rep(out, cch, used, "  [失败] %s\n         实际: %s\n         期望: %s\n",
            what, got, want);
    }
}

static unsigned char *pseudo_data(size_t n, unsigned int seed)
{
    unsigned char *p = (unsigned char *)malloc(n ? n : 1);
    unsigned int s = seed ? seed : 1;
    size_t i;
    for (i = 0; i < n; i++) {
        s = s * 1103515245u + 12345u;
        p[i] = (unsigned char)((s >> 16) & 0xFF);
    }
    return p;
}

int b64sha_selftest(char *report, size_t cch)
{
    size_t used = 0;
    char hex[65], enc[256];
    unsigned char out[256];

    g_pass = g_fail = 0;
    report[0] = '\0';

    rep(report, cch, &used,
        "编码工具箱 · 内置自检\n"
        "============================\n");

    /* ---------- Base64 ---------- */
    b64_encode((const unsigned char *)"", 0, enc);
    eq(report, cch, &used, enc, "", "Base64 空输入");
    b64_encode((const unsigned char *)"f", 1, enc);
    eq(report, cch, &used, enc, "Zg==", "Base64 \"f\"");
    b64_encode((const unsigned char *)"fo", 2, enc);
    eq(report, cch, &used, enc, "Zm8=", "Base64 \"fo\"");
    b64_encode((const unsigned char *)"foo", 3, enc);
    eq(report, cch, &used, enc, "Zm9v", "Base64 \"foo\"");
    b64_encode((const unsigned char *)"foob", 4, enc);
    eq(report, cch, &used, enc, "Zm9vYg==", "Base64 \"foob\"");
    b64_encode((const unsigned char *)"fooba", 5, enc);
    eq(report, cch, &used, enc, "Zm9vYmE=", "Base64 \"fooba\"");
    b64_encode((const unsigned char *)"foobar", 6, enc);
    eq(report, cch, &used, enc, "Zm9vYmFy", "Base64 \"foobar\"");

    /* 中文 UTF-8 */
    {
        const char *cn = "\xe6\xb1\x89\xe5\xad\x97";   /* 汉字 */
        b64_encode((const unsigned char *)cn, 6, enc);
        eq(report, cch, &used, enc, "5rGJ5a2X", "Base64 中文\"汉字\"");
        ok(report, cch, &used,
           b64_decode(enc, strlen(enc), out, sizeof(out)) == 6 &&
           memcmp(out, cn, 6) == 0, "中文 UTF-8 往返一致");
    }

    /* 宽松解码 */
    ok(report, cch, &used,
       b64_decode("Zm9v\r\nYmFy", 10, out, sizeof(out)) == 6 &&
       memcmp(out, "foobar", 6) == 0, "忽略回车换行解码");
    ok(report, cch, &used,
       b64_decode("Zm9v YmFy", 9, out, sizeof(out)) == 6, "忽略空格解码");
    ok(report, cch, &used,
       b64_decode("-_8", 3, out, sizeof(out)) == 2, "兼容 URL-safe 字母表");

    /* 往返一致性 + 大缓冲区 */
    {
        static const size_t lens[] = { 0, 1, 2, 3, 4, 5, 63, 64, 65, 1024, 65536 };
        size_t i, bad = 0;
        for (i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
            size_t n = lens[i];
            unsigned char *raw = pseudo_data(n, (unsigned int)(n * 7 + 3));
            size_t el = b64_encoded_len(n);
            char *e = (char *)malloc(el + 1);
            unsigned char *d = (unsigned char *)malloc(n + 16);
            size_t dl;
            b64_encode(raw, n, e);
            dl = b64_decode(e, el, d, n + 16);
            if (dl != n || (n && memcmp(raw, d, n) != 0))
                bad++;
            free(raw); free(e); free(d);
        }
        ok(report, cch, &used, bad == 0, "多长度往返一致性");
    }
    {
        size_t n = 1024 * 1024;
        unsigned char *raw = pseudo_data(n, 999);
        size_t el = b64_encoded_len(n);
        char *e = (char *)malloc(el + 1);
        unsigned char *d = (unsigned char *)malloc(n + 16);
        size_t dl;
        b64_encode(raw, n, e);
        dl = b64_decode(e, el, d, n + 16);
        ok(report, cch, &used, dl == n && memcmp(raw, d, n) == 0,
           "1MB 数据往返一致");
        free(raw); free(e); free(d);
    }

    /* ---------- SHA-256 ---------- */
    sha256_text("", 0, hex, 0);
    eq(report, cch, &used, hex,
       "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
       "SHA256 空串");
    sha256_text("abc", 3, hex, 0);
    eq(report, cch, &used, hex,
       "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
       "SHA256 \"abc\"");
    sha256_text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hex, 0);
    eq(report, cch, &used, hex,
       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
       "SHA256 448bit 官方向量");

    /* 补位边界 */
    {
        static const struct { size_t n; const char *want; } c[] = {
            { 55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318" },
            { 56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a" },
            { 57, "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6" },
            { 64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb" },
        };
        size_t i;
        for (i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
            char *buf = (char *)malloc(c[i].n + 1);
            char what[64];
            memset(buf, 'a', c[i].n);
            sha256_text(buf, c[i].n, hex, 0);
            snprintf(what, sizeof(what), "SHA256 %d 个 'a'（补位边界）", (int)c[i].n);
            eq(report, cch, &used, hex, c[i].want, what);
            free(buf);
        }
    }

    /* 百万字节流式 */
    {
        sha256_ctx ctx;
        unsigned char digest[32];
        char big[1000];
        int i;
        memset(big, 'a', sizeof(big));
        sha256_init(&ctx);
        for (i = 0; i < 1000; i++)
            sha256_update(&ctx, big, sizeof(big));
        sha256_final(&ctx, digest);
        sha256_hex(digest, hex, 0);
        eq(report, cch, &used, hex,
           "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
           "SHA256 100 万个 'a'（分块流式）");
    }

    /* 不规则分块一致性 */
    {
        size_t n = 123457, off = 0, k = 0;
        unsigned char *raw = pseudo_data(n, 2024);
        char one[65], chunk[65];
        static const size_t steps[] = { 1, 2, 3, 63, 64, 65, 1000, 4096, 65536 };
        sha256_ctx ctx;
        unsigned char digest[32];

        sha256_text(raw, n, one, 0);
        sha256_init(&ctx);
        while (off < n) {
            size_t step = steps[k % (sizeof(steps) / sizeof(steps[0]))];
            if (step > n - off) step = n - off;
            sha256_update(&ctx, raw + off, step);
            off += step;
            k++;
        }
        sha256_final(&ctx, digest);
        sha256_hex(digest, chunk, 0);
        eq(report, cch, &used, chunk, one, "SHA256 不规则分块结果一致");
        free(raw);
    }

    /* 大写输出 */
    sha256_text("abc", 3, hex, 1);
    ok(report, cch, &used,
       strcmp(hex, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD") == 0,
       "SHA256 大写输出");

    /* ---------- 文件类型识别 ---------- */
    {
        static const struct { const char *sig; size_t n; const char *want; } c[] = {
            { "\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR", 16, ".png" },
            { "\xff\xd8\xff\xe0\x00\x10JFIF", 10, ".jpg" },
            { "GIF89a\x01\x00\x01\x00", 10, ".gif" },
            { "%PDF-1.7\n%\xe2\xe3\xcf\xd3", 14, ".pdf" },
            { "PK\x03\x04\x14\x00\x00\x00\x08\x00", 12, ".zip" },
            { "hello world\nplain text\n", 22, ".txt" },
            { "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09", 10, ".bin" },
        };
        size_t i, bad = 0;
        for (i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
            const char *d = NULL;
            const char *e = detect_file_ext((const unsigned char *)c[i].sig, c[i].n, &d);
            if (strcmp(e, c[i].want) != 0) {
                bad++;
                rep(report, cch, &used, "  [失败] 类型识别 期望 %s 实际 %s\n",
                    c[i].want, e);
            } else {
                g_pass++;
            }
        }
        ok(report, cch, &used, bad == 0, "文件类型识别（魔数）");
    }

    /* v2.0 新增模块的独立自检：各自输出自己的报告与计数 */
    rep(report, cch, &used, "\n");
    {
        size_t mark = used;
        int f = jl_selftest(report + used, cch - used);
        used = mark + strlen(report + mark);
        g_fail += f;
    }
    {
        size_t mark = used;
        int f = api_selftest(report + used, cch - used);
        used = mark + strlen(report + mark);
        g_fail += f;
    }

    rep(report, cch, &used,
        "\n============================\n"
        "算法断言 %d 项（不含上方两个子模块的独立计数），失败 %d 项\n",
        g_pass + g_fail, g_fail);
    rep(report, cch, &used,
        g_fail == 0 ? "结论：全部通过，本机运行正常。\n"
                    : "结论：存在失败项，请勿使用该文件。\n");

    return g_fail;
}
