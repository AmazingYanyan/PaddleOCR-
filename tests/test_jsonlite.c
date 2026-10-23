/*
 * test_jsonlite.c - 容错 JSON 扫描器单元测试（宿主机直接跑，不需要 Windows）
 *
 * 覆盖重点：
 *   · 字段提取的边界（数字/字符串/负数/嵌套同名 key/字符串里出现的假 key）
 *   · 图片判定的四重条件（尤其"不该误判"的反例）
 *   · 脱敏副本仍是合法 JSON
 *   · 预览截断不落在字符串中间
 *   · 畸形输入（HTML、截断、深嵌套、纯垃圾）不崩不死循环
 *
 * 编译运行：
 *   cc -std=c99 -O1 -g -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion \
 *      -fsanitize=address,undefined -o build/test_jsonlite \
 *      tests/test_jsonlite.c src/jsonlite.c src/b64sha.c
 *   ./build/test_jsonlite
 */
#include "../src/jsonlite.h"
#include "../src/b64sha.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;

static void ok(int cond, const char *what)
{
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        printf("  [失败] %s\n", what);
    }
}

static void eqs(const char *got, const char *want, const char *what)
{
    if (got && want && strcmp(got, want) == 0) {
        g_pass++;
    } else {
        g_fail++;
        printf("  [失败] %s\n         实际: %s\n         期望: %s\n",
               what, got ? got : "(null)", want ? want : "(null)");
    }
}

/* 造一段"像某种图片"的 Base64：给定魔数 + 补零到 total 字节，再编码 */
static void make_image_b64(const unsigned char *magic, size_t magic_len,
                           size_t total, char *out, size_t outcap)
{
    unsigned char *raw = (unsigned char *)calloc(total, 1);
    size_t need;

    memcpy(raw, magic, magic_len);
    need = b64_encoded_len(total) + 1;
    if (need > outcap) {
        printf("  [内部错误] make_image_b64 缓冲不足\n");
        free(raw);
        out[0] = '\0';
        return;
    }
    b64_encode(raw, total, out);
    free(raw);
}

static long g_int;
static char g_str[256];

static void t_basic(void)
{
    const char *j = "{\"logId\":\"abc\",\"errorCode\":0,\"errorMsg\":\"Success\","
                    "\"result\":{\"page_count\":1}}";
    jl_summary s;

    ok(jl_find_int(j, strlen(j), "errorCode", &g_int) == 1 &&
       g_int == 0, "取整数 errorCode = 0");

    ok(jl_find_str(j, strlen(j), "errorMsg", g_str, sizeof(g_str)) == 1,
       "取字符串 errorMsg 成功");
    eqs(g_str, "Success", "errorMsg 内容正确");

    ok(jl_find_str(j, strlen(j), "logId", g_str, sizeof(g_str)) == 1, "取 logId");
    eqs(g_str, "abc", "logId 内容正确");

    ok(jl_find_int(j, strlen(j), "page_count", &g_int) == 1 && g_int == 1,
       "取嵌套对象内的整数");

    ok(jl_find_int(j, strlen(j), "noSuchKey", &g_int) == 0,
       "不存在的 key 返回 0");

    jl_scan(j, strlen(j), NULL, 0, &s);
    ok(s.images.count == 0, "普通响应没有图片");
    jl_summary_free(&s);
}

static void t_numbers(void)
{
    const char *j;

    j = "{\"errorCode\":\"10001\"}";
    ok(jl_find_int(j, strlen(j), "errorCode", &g_int) == 1 &&
       g_int == 10001, "字符串形式的数字能取值");

    j = "{\"errorCode\":-1}";
    ok(jl_find_int(j, strlen(j), "errorCode", &g_int) == 1 &&
       g_int == -1, "负数能取值");

    j = "{\"errorCode\":0 }";
    ok(jl_find_int(j, strlen(j), "errorCode", &g_int) == 1 && g_int == 0,
       "键与冒号间有空白");

    j = "{ \"errorCode\" : 7 }";
    ok(jl_find_int(j, strlen(j), "errorCode", &g_int) == 1 && g_int == 7,
       "冒号两侧有空白");

    j = "{\"errorCode\":true}";
    ok(jl_find_int(j, strlen(j), "errorCode", &g_int) == 0,
       "布尔值不被当作数字");
}

static void t_tricky_keys(void)
{
    const char *j;

    /* 字符串值里出现 "errorCode":42 不应被误取 */
    j = "{\"errorMsg\":\"\\\"errorCode\\\":42\",\"errorCode\":7}";
    ok(jl_find_int(j, strlen(j), "errorCode", &g_int) == 1 && g_int == 7,
       "字符串里的假 key 不会抢先命中");

    /* 嵌套里的同名 key 不覆盖外层首次值 */
    j = "{\"errorCode\":0,\"result\":{\"errorCode\":99}}";
    ok(jl_find_int(j, strlen(j), "errorCode", &g_int) == 1 && g_int == 0,
       "嵌套同名 key 取外层首次值");

    /* key 出现在数组元素里 */
    j = "{\"list\":[{\"errorMsg\":\"inner\"}],\"errorMsg\":\"outer\"}";
    ok(jl_find_str(j, strlen(j), "errorMsg", g_str, sizeof(g_str)) == 1,
       "数组内 key 也能取到");
}

static void t_escapes(void)
{
    const char *j;

    j = "{\"errorMsg\":\"a\\\"b\\\\c/d\"}";
    ok(jl_find_str(j, strlen(j), "errorMsg", g_str, sizeof(g_str)) == 1,
       "含转义字符能取值");
    eqs(g_str, "a\"b\\c/d", "转义还原正确");

    j = "{\"errorMsg\":\"\\u53c2\\u6570\\u9519\\u8bef\"}";
    jl_find_str(j, strlen(j), "errorMsg", g_str, sizeof(g_str));
    eqs(g_str, "参数错误", "\\uXXXX 中文还原");

    j = "{\"errorMsg\":\"\\ud83d\\ude00\"}";
    jl_find_str(j, strlen(j), "errorMsg", g_str, sizeof(g_str));
    ok(strcmp(g_str, "\xF0\x9F\x98\x80") == 0, "代理对合成 4 字节 UTF-8");

    j = "{\"errorMsg\":\"line1\\nline2\\ttab\"}";
    jl_find_str(j, strlen(j), "errorMsg", g_str, sizeof(g_str));
    eqs(g_str, "line1\nline2\ttab", "\\n \\t 还原");

    /* 中文原样（未转义）应保持不变 */
    j = "{\"errorMsg\":\"接口超时\"}";
    jl_find_str(j, strlen(j), "errorMsg", g_str, sizeof(g_str));
    eqs(g_str, "接口超时", "未转义中文原样保留");
}

static void t_bom_and_truncated(void)
{
    char j[512];
    const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };

    memcpy(j, bom, 3);
    strcpy(j + 3, "{\"errorCode\":0,\"errorMsg\":\"ok\"}");
    ok(jl_is_json(j, strlen(j)) == 1, "带 BOM 仍识别为 JSON");
    ok(jl_find_int(j, strlen(j), "errorCode", &g_int) == 1 && g_int == 0,
       "带 BOM 仍能取字段");

    /* 截断到一半：至少 errorCode 要能取到，且不能崩 */
    strcpy(j, "{\"errorCode\":0,\"errorMsg\":\"unterminat");
    ok(jl_find_int(j, strlen(j), "errorCode", &g_int) == 1 && g_int == 0,
       "截断 JSON 仍能取到前面的字段");
    ok(jl_find_str(j, strlen(j), "errorMsg", g_str, sizeof(g_str)) == 1,
       "未闭合字符串被当作内容（容错）");

    /* 末尾多逗号 */
    strcpy(j, "{\"errorCode\":1,}");
    ok(jl_find_int(j, strlen(j), "errorCode", &g_int) == 1 && g_int == 1,
       "末尾多余逗号不影响取值");
}

static void t_deep(void)
{
    /* 100 层嵌套：超过深度上限后应跳过而不是爆栈 */
    char j[4096];
    size_t i, o = 0;
    for (i = 0; i < 100; i++) j[o++] = '[';
    j[o++] = '1';
    for (i = 0; i < 100; i++) j[o++] = ']';
    j[o] = '\0';

    ok(jl_is_json(j, o) == 1, "深嵌套输入被识别为 JSON");
    {
        jl_image_list l;
        ok(jl_scan_images(j, o, &l) == 0, "深嵌套不崩溃（不爆栈）");
        ok(l.count == 0, "深嵌套无图片");
        jl_image_list_free(&l);
    }
}

static void t_images(void)
{
    static const unsigned char png[] = { 0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A };
    static const unsigned char jpg[] = { 0xFF,0xD8,0xFF,0xE0,0x00,0x10,'J','F' };
    static const unsigned char gif[] = { 'G','I','F','8','9','a' };
    static const unsigned char bmp[] = { 'B','M' };
    char b64png[1024], b64jpg[1024], b64gif[1024], b64bmp[1024];
    char j[8192];
    jl_image_list l;

    make_image_b64(png, sizeof(png), 200, b64png, sizeof(b64png));
    make_image_b64(jpg, sizeof(jpg), 200, b64jpg, sizeof(b64jpg));
    make_image_b64(gif, sizeof(gif), 200, b64gif, sizeof(b64gif));
    make_image_b64(bmp, sizeof(bmp), 200, b64bmp, sizeof(b64bmp));

    ok(strlen(b64png) >= 256, "测试用 PNG Base64 长度超过判定门槛");

    /* 单张 PNG，路径应含数组下标 */
    snprintf(j, sizeof(j),
             "{\"result\":{\"layoutParsingResults\":[{\"outputImages\":"
             "{\"layout_det_res\":\"%s\"}}]}}", b64png);
    ok(jl_scan_images(j, strlen(j), &l) == 0, "scan_images 成功");
    ok(l.count == 1, "识别出 1 张图片");
    if (l.count == 1) {
        eqs(l.items[0].ext, ".png", "按魔数判定为 .png");
        ok(strstr(l.items[0].path, "layoutParsingResults[0]") != NULL,
           "路径含数组下标");
        ok(strstr(l.items[0].path, "layout_det_res") != NULL, "路径含键名");
    }
    jl_image_list_free(&l);

    /* 四种格式 */
    snprintf(j, sizeof(j), "{\"a\":\"%s\",\"b\":\"%s\",\"c\":\"%s\",\"d\":\"%s\"}",
             b64png, b64jpg, b64gif, b64bmp);
    ok(jl_scan_images(j, strlen(j), &l) == 0, "scan_images(四格式)");
    ok(l.count == 4, "四种格式全部识别");
    if (l.count == 4) {
        eqs(l.items[0].ext, ".png", "第 1 张 .png");
        eqs(l.items[1].ext, ".jpg", "第 2 张 .jpg");
        eqs(l.items[2].ext, ".gif", "第 3 张 .gif");
        eqs(l.items[3].ext, ".bmp", "第 4 张 .bmp");
    }
    jl_image_list_free(&l);

    /* data URL 前缀 */
    snprintf(j, sizeof(j), "{\"a\":\"data:image/png;base64,%s\"}", b64png);
    ok(jl_scan_images(j, strlen(j), &l) == 0, "scan_images(data URL)");
    ok(l.count == 1, "带 data URL 前缀的能识别");
    if (l.count == 1) {
        ok(l.items[0].prefix_len > 0, "记录了前缀长度");
        {
            size_t plen = 0;
            const char *p = jl_image_payload_ptr(j, &l.items[0], &plen);
            ok(p != NULL && plen == strlen(b64png), "payload 跳过 data 前缀");
        }
    }
    jl_image_list_free(&l);

    /* data URL 用 jpeg 的 MIME，但魔数是 PNG：以魔数为准 */
    snprintf(j, sizeof(j), "{\"a\":\"data:image/jpeg;base64,%s\"}", b64png);
    jl_scan_images(j, strlen(j), &l);
    ok(l.count == 1 && strcmp(l.items[0].ext, ".png") == 0, "MIME 与魔数冲突时以魔数为准");
    jl_image_list_free(&l);

    /* b64 字符数统计 */
    snprintf(j, sizeof(j), "{\"a\":\"%s\"}", b64png);
    {
        jl_summary s;
        jl_scan(j, strlen(j), NULL, 0, &s);
        ok(s.images_b64_chars == strlen(b64png), "统计的 Base64 字符数正确");
        jl_summary_free(&s);
    }
}

static void t_false_positive(void)
{
    char j[8192];
    jl_image_list l;
    size_t i;

    /* 1) 长 Markdown 文本（字母为主，长度是 4 的倍数也一样） */
    {
        char big[1200];
        for (i = 0; i < sizeof(big) - 1; i++)
            big[i] = (char)('a' + (char)(i % 26));
        big[sizeof(big) - 1] = '\0';
        snprintf(j, sizeof(j), "{\"markdown\":{\"text\":\"%s\"}}", big);
        jl_scan_images(j, strlen(j), &l);
        ok(l.count == 0, "长 Markdown 文本不误判为图片");
        jl_image_list_free(&l);
    }

    /* 2) 一长串 'A'：Base64 合法，但解出来不是图片 */
    {
        char as[600];
        memset(as, 'A', sizeof(as) - 1);
        as[sizeof(as) - 1] = '\0';
        snprintf(j, sizeof(j), "{\"k\":\"%s\"}", as);
        jl_scan_images(j, strlen(j), &l);
        ok(l.count == 0, "全 A 长串不误判（魔数不匹配）");
        jl_image_list_free(&l);
    }

    /* 3) 截断的 Base64（长度不是 4 的倍数） */
    {
        static const unsigned char png[] = { 0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A };
        char b64[1024], cut[1024];
        make_image_b64(png, sizeof(png), 200, b64, sizeof(b64));
        snprintf(cut, sizeof(cut), "%s", b64);
        cut[strlen(cut) - 1] = '\0';
        snprintf(j, sizeof(j), "{\"k\":\"%s\"}", cut);
        jl_scan_images(j, strlen(j), &l);
        ok(l.count == 0, "截断的 Base64 不误判");
        jl_image_list_free(&l);
    }

    /* 4) 含空格/换行的长串 */
    {
        static const unsigned char png[] = { 0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A };
        char b64[1024], spaced[1200];
        make_image_b64(png, sizeof(png), 200, b64, sizeof(b64));
        snprintf(spaced, sizeof(spaced), "%.80s %.80s", b64, b64 + 80);
        snprintf(j, sizeof(j), "{\"k\":\"%s\"}", spaced);
        jl_scan_images(j, strlen(j), &l);
        ok(l.count == 0, "含空格的串不当作图片");
        jl_image_list_free(&l);
    }

    /* 5) 短字符串（低于 256 门槛） */
    {
        static const unsigned char png[] = { 0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A };
        char b64[1024];
        make_image_b64(png, sizeof(png), 30, b64, sizeof(b64));
        snprintf(j, sizeof(j), "{\"k\":\"%s\"}", b64);
        jl_scan_images(j, strlen(j), &l);
        ok(l.count == 0, "过短的 Base64 不当图片");
        jl_image_list_free(&l);
    }
}

static void t_redact(void)
{
    static const unsigned char png[] = { 0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A };
    char b64[1024];
    char j[4096];
    jl_image_list l;
    char *red = NULL;
    size_t redlen = 0;
    jl_summary s;

    make_image_b64(png, sizeof(png), 200, b64, sizeof(b64));

    snprintf(j, sizeof(j),
             "{\"errorCode\":0,\"errorMsg\":\"Success\",\"result\":"
             "{\"inputImage\":\"%s\",\"outputImages\":{\"a\":\"%s\"},"
             "\"markdown\":{\"text\":\"正文内容\"}}}", b64, b64);

    jl_scan_images(j, strlen(j), &l);
    ok(l.count == 2, "脱敏用例识别出 2 张图");

    ok(jl_redact(j, strlen(j), &l, &red, &redlen) == 0, "jl_redact 成功");
    ok(red != NULL, "脱敏结果非空");
    if (red) {
        ok(strstr(red, b64) == NULL, "脱敏后不含原始 Base64");
        ok(strstr(red, "\"...\"") != NULL, "被替换为 \"...\"");
        ok(strstr(red, "正文内容") != NULL, "普通文本保留");
        ok(strstr(red, "\"errorCode\":0") != NULL, "数字字段保留");
        eqs(strstr(red, "inputImage") ? "y" : "n", "y", "图片的键名仍保留");
        ok(redlen < strlen(j), "脱敏副本比原文短");
        ok(jl_is_json(red, redlen) == 1, "脱敏副本仍是 JSON");

        /* 脱敏后再次扫描：应识别不到任何图片 */
        jl_scan(red, redlen, NULL, 0, &s);
        ok(s.images.count == 0, "脱敏后再扫描无图片");
        jl_summary_free(&s);

        free(red);
    }
    jl_image_list_free(&l);

    /* 没有图片时，脱敏等价于原样复制 */
    {
        const char *plain = "{\"a\":1,\"b\":\"text\"}";
        char *r2 = NULL;
        size_t n2 = 0;
        jl_image_list empty;
        memset(&empty, 0, sizeof(empty));
        ok(jl_redact(plain, strlen(plain), &empty, &r2, &n2) == 0, "无图片时脱敏成功");
        eqs(r2, plain, "无图片时内容不变");
        free(r2);
    }
}

static void t_truncate(void)
{
    const char *j = "{\"a\":\"aaaaaaaaaa\",\"b\":\"bbbbbbbbbb\",\"c\":\"cccccccccc\"}";
    char *t = NULL;
    size_t n = 0;
    int trunc = 0;

    ok(jl_truncate_preview(j, strlen(j), 200, &t, &n, &trunc) == 0, "短内容截断调用成功");
    ok(trunc == 0 && n == strlen(j), "未超过 cap 时不截断");
    free(t);

    t = NULL;
    ok(jl_truncate_preview(j, strlen(j), 30, &t, &n, &trunc) == 0, "截断调用成功");
    ok(trunc == 1, "超过 cap 时标记已截断");
    ok(n <= 30, "截断结果不超长");
    if (t) {
        /* 截断点应在一个逗号处，而不是字符串中间 */
        ok(t[n - 1] == ',' || n == 30, "截断优先落在逗号处");
        free(t);
    }
}

static void t_junk(void)
{
    static const char *junk[] = {
        "",
        "{",
        "[",
        "[[[[[[",
        "}{",
        "not json",
        "<html><body>502 Bad Gateway</body></html>",
        "{\"a\": }",
        "{\"a\" \"b\"}",
        "{\"a\":\"\\\"",
        "{\"a\":[1,2,3,]}",
        "null",
        "12345",
        "{\"file\":\"AAAAAAAA\"}",
    };
    size_t i;
    int all_ok = 1;

    for (i = 0; i < sizeof(junk) / sizeof(junk[0]); i++) {
        jl_image_list l;
        jl_summary s;
        if (jl_scan(junk[i], strlen(junk[i]), NULL, 0, &s) != 0)
            all_ok = 0;
        if (s.images.count != 0)
            all_ok = 0;
        jl_summary_free(&s);
        if (jl_scan_images(junk[i], strlen(junk[i]), &l) != 0)
            all_ok = 0;
        jl_image_list_free(&l);
    }
    ok(all_ok, "14 种畸形输入均不崩溃、不误判");

    ok(jl_is_json("", 0) == 0, "空串不是 JSON");
    ok(jl_is_json("   ", 3) == 0, "纯空白不是 JSON");
    ok(jl_is_json("null", 4) == 0, "null 不是对象/数组");
    ok(jl_is_json("{}", 2) == 1, "{} 是 JSON");
}

static void t_unescape_unit(void)
{
    char *u = NULL;
    size_t n = 0;

    ok(jl_unescape("abc", 3, &u, &n) == 0 && strcmp(u, "abc") == 0, "无转义原样");
    free(u);

    ok(jl_unescape("a\\nb", 4, &u, &n) == 0 && strcmp(u, "a\nb") == 0, "\\n 转义");
    free(u);

    ok(jl_unescape("\\u0041", 6, &u, &n) == 0 && strcmp(u, "A") == 0, "\\u0041 -> A");
    free(u);

    /* 非法 \u 应原样保留而不是崩 */
    ok(jl_unescape("\\uZZZZ", 6, &u, &n) == 0, "非法 \\u 不崩溃");
    free(u);

    /* 结尾孤立反斜杠 */
    ok(jl_unescape("ab\\", 3, &u, &n) == 0, "孤立反斜杠不崩溃");
    free(u);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("==== jsonlite 单元测试 ====\n");

    printf("-- 基础字段 --\n");   t_basic();
    printf("-- 数字 --\n");       t_numbers();
    printf("-- 狡猾的 key --\n"); t_tricky_keys();
    printf("-- 转义 --\n");       t_escapes();
    printf("-- BOM/截断 --\n");   t_bom_and_truncated();
    printf("-- 深嵌套 --\n");     t_deep();
    printf("-- 图片识别 --\n");   t_images();
    printf("-- 反误判 --\n");     t_false_positive();
    printf("-- 脱敏 --\n");       t_redact();
    printf("-- 预览截断 --\n");   t_truncate();
    printf("-- 畸形输入 --\n");   t_junk();
    printf("-- 反转义 --\n");     t_unescape_unit();

    printf("\n---- 结论 ----\n");
    printf("通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    if (g_fail == 0)
        printf("结果：ALL PASS\n");
    return g_fail ? 1 : 0;
}
