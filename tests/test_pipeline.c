/*
 * test_pipeline.c — 编码链路回归测试（仅依赖 b64sha.c，可在 macOS/Linux 上直接运行）
 *
 * 覆盖用户实际场景："选一张 400KB 的图片 → 转 Base64 → 按 76 字符换行 → 再解码回来"。
 * 这里不涉及任何 Windows API（文件读写由 read_whole_file 承担，只能在目标机验证），
 * 验证的是这条链路上所有纯算法环节：
 *     原始字节
 *       -> b64_encoded_len / b64_encode      期望 4*ceil(n/3)
 *       -> wrap_text 等价实现（每 76 字符插入 CRLF）
 *       -> b64_decode（自动忽略空格与换行）
 *       -> 与原字节逐字节比对
 *
 * 编译运行：
 *     cc -std=c99 -O2 -Wall -Wextra -o build/test_pipeline tests/test_pipeline.c src/b64sha.c
 *     ./build/test_pipeline
 */
#include "../src/b64sha.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define B64_LINE_WIDTH 76

static int g_pass = 0;
static int g_fail = 0;

static void ok(const char *what)
{
    g_pass++;
    (void)what;
}

static void bad(const char *what, const char *detail)
{
    g_fail++;
    printf("  [失败] %s  %s\n", what, detail);
}

/* 与 main.c 中 wrap_text 完全相同的算法（每 width 个字符插入 CRLF） */
static char *wrap_dup(const char *src, size_t srcLen, size_t width, size_t *outLen)
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
        return NULL;
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

/* 跑一遍完整链路 */
static void roundtrip(size_t n, unsigned int seed, int wrap)
{
    unsigned char *raw;
    char *b64, *shown;
    unsigned char *back;
    size_t encLen, shownLen = 0, cap, dn, i, boxData;
    char label[128];

    raw = (unsigned char *)malloc(n ? n : 1);
    for (i = 0; i < n; i++) {                     /* 确定性伪随机，可复现 */
        seed = seed * 1103515245u + 12345u;
        raw[i] = (unsigned char)((seed >> 16) & 0xFF);
    }

    /* --- 编码 --- */
    encLen = b64_encoded_len(n);
    b64 = (char *)malloc(encLen + 1);
    b64_encode(raw, n, b64);

    snprintf(label, sizeof(label), "n=%zu 换行=%d", n, wrap);

    if (strlen(b64) != encLen) {
        bad(label, "Base64 长度与 b64_encoded_len 不一致");
        free(b64); free(raw);
        return;
    }

    /* --- 换行（模拟文本框里的呈现） --- */
    if (wrap) {
        shown = wrap_dup(b64, encLen, B64_LINE_WIDTH, &shownLen);
        if (!shown) {
            bad(label, "换行缓冲分配失败");
            free(b64); free(raw);
            return;
        }
    } else {
        shownLen = encLen;
        shown = (char *)malloc(encLen + 1);
        memcpy(shown, b64, encLen + 1);
    }

    /* --- 模拟"文本框回读"：统计非空白字符数，应等于 Base64 长度 --- */
    {
        unsigned char *raw2 = (unsigned char *)malloc(n ? n : 1);
        size_t got = b64_decode(b64, encLen, raw2, n ? n : 1);
        if (got != n || (n && memcmp(raw2, raw, n) != 0))
            bad(label, "规范串解码后与原始数据不一致");
        else
            ok(label);
        free(raw2);
    }

    /* --- 换行串解码（应自动忽略 CRLF） --- */
    boxData = 0;
    for (i = 0; i < shownLen; i++) {
        char c = shown[i];
        if (c != '\r' && c != '\n' && c != ' ' && c != '\t')
            boxData++;
    }
    if (boxData != encLen)
        bad(label, "换行串中的有效 Base64 字符数不等于原始长度");

    cap = b64_decoded_cap(shownLen);
    back = (unsigned char *)malloc(cap ? cap : 1);
    dn = b64_decode(shown, shownLen, back, cap);
    if (dn == (size_t)-1) {
        bad(label, "换行串无法解码");
    } else if (dn != n) {
        char d[128];
        snprintf(d, sizeof(d), "解码长度 %zu != 原始长度 %zu", dn, n);
        bad(label, d);
    } else if (n && memcmp(back, raw, n) != 0) {
        bad(label, "换行串解码后与原始数据不一致");
    } else {
        ok(label);
    }

    free(back);
    free(shown);
    free(b64);
    free(raw);
}

int main(void)
{
    /* 边界与常见尺寸；400KB 正是用户报告出问题的图片大小 */
    static const size_t sizes[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8,
        55, 56, 57, 58, 63, 64, 65,
        75, 76, 77, 151, 152, 153,
        4095, 4096, 4097,
        1024 * 100, 1024 * 200, 1024 * 400, 1024 * 400 + 1, 1024 * 512 + 7
    };
    size_t k;

    printf("==== 编码链路回归测试（编码 -> 换行 -> 回读解码）====\n");
    for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        roundtrip(sizes[k], (unsigned int)(20261013u + k), 1);   /* 换行开 */
        roundtrip(sizes[k], (unsigned int)(70000000u + k), 0);   /* 换行关 */
    }

    printf("\n---- 结论 ----\n");
    printf("通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    if (g_fail == 0)
        printf("结果：ALL PASS\n");
    return g_fail ? 1 : 0;
}
