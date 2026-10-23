/*
 * test_core.c - b64sha 核心算法自检程序
 *
 * 用法：
 *   ./test_core                    # 跑内置标准测试向量 + 往返一致性测试
 *   ./test_core vectors.txt        # 额外校验由 Python 生成的随机对拍样本
 *
 * 退出码 0 表示全部通过。
 */
#include "../src/b64sha.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail = 0;
static int g_total = 0;

static void check(int cond, const char *what)
{
    g_total++;
    if (!cond) {
        g_fail++;
        printf("  [FAIL] %s\n", what);
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    g_total++;
    if (strcmp(got, want) != 0) {
        g_fail++;
        printf("  [FAIL] %s\n         got : %s\n         want: %s\n", what, got, want);
    }
}

/* -------- 确定性伪随机（LCG），保证可复现 -------- */
static unsigned char *make_data(size_t n, unsigned int seed)
{
    unsigned char *p = (unsigned char *)malloc(n ? n : 1);
    unsigned int s = seed ? seed : 12345u;
    size_t i;
    for (i = 0; i < n; i++) {
        s = s * 1103515245u + 12345u;
        p[i] = (unsigned char)((s >> 16) & 0xFF);
    }
    return p;
}

static void test_base64_vectors(void)
{
    struct { const char *in; const char *want; } v[] = {
        { "",       ""         },
        { "f",      "Zg=="     },
        { "fo",     "Zm8="     },
        { "foo",    "Zm9v"     },
        { "foob",   "Zm9vYg==" },
        { "fooba",  "Zm9vYmE=" },
        { "foobar", "Zm9vYmFy" },
    };
    size_t i;
    char buf[64];

    printf("[1] Base64 标准测试向量（RFC 4648）\n");
    for (i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        b64_encode((const unsigned char *)v[i].in, strlen(v[i].in), buf);
        check_str(buf, v[i].want, v[i].in);
    }

    /* 边界长度 */
    printf("[2] Base64 长度边界与往返一致性\n");
    {
        size_t lens[] = { 0, 1, 2, 3, 4, 5, 6, 7, 60, 61, 62, 63, 64, 65, 66,
                          127, 128, 129, 255, 256, 1024, 65535, 65536 };
        for (i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
            size_t n = lens[i];
            unsigned char *raw = make_data(n, (unsigned int)(n * 7 + 3));
            size_t elen = b64_encoded_len(n);
            char *enc = (char *)malloc(elen + 1);
            unsigned char *dec = (unsigned char *)malloc(n + 16);
            size_t dlen;

            b64_encode(raw, n, enc);
            check(strlen(enc) == elen, "编码长度与预测一致");
            check((elen % 4) == 0, "编码长度是 4 的倍数");

            dlen = b64_decode(enc, strlen(enc), dec, n + 16);
            check(dlen == n, "解码长度等于原长度");
            check(n == 0 || memcmp(raw, dec, n) == 0, "解码内容与原内容逐字节相同");

            free(raw); free(enc); free(dec);
        }
    }

    /* 大文件路径（1MB / 4MB） */
    printf("[3] Base64 大缓冲区（1MB / 4MB）\n");
    {
        size_t n = 1024 * 1024;
        unsigned char *raw = make_data(n, 999);
        size_t elen = b64_encoded_len(n);
        char *enc = (char *)malloc(elen + 1);
        unsigned char *dec = (unsigned char *)malloc(n + 16);
        size_t dlen;

        b64_encode(raw, n, enc);
        dlen = b64_decode(enc, elen, dec, n + 16);
        check(dlen == n && memcmp(raw, dec, n) == 0, "1MB 往返一致");
        free(raw); free(enc); free(dec);

        n = 4 * 1024 * 1024;
        raw = make_data(n, 4242);
        elen = b64_encoded_len(n);
        enc = (char *)malloc(elen + 1);
        dec = (unsigned char *)malloc(n + 16);
        b64_encode(raw, n, enc);
        dlen = b64_decode(enc, elen, dec, n + 16);
        check(dlen == n && memcmp(raw, dec, n) == 0, "4MB 往返一致");
        free(raw); free(enc); free(dec);
    }

    /* 宽松解码：空白、换行、URL-safe、填充 */
    printf("[4] Base64 宽松解码（换行 / 空格 / URL-safe）\n");
    {
        unsigned char out[64];
        size_t n;
        n = b64_decode("Zm9v\r\nYmFy", 10, out, sizeof(out));
        check(n == 6 && memcmp(out, "foobar", 6) == 0, "忽略回车换行");

        n = b64_decode("Zm9v YmFy", 9, out, sizeof(out));
        check(n == 6 && memcmp(out, "foobar", 6) == 0, "忽略空格");

        /* "汉字" 的 UTF-8 字节 -> 6Lil5a2X */
        n = b64_decode("5rGJ5a2X", 8, out, sizeof(out));
        check(n == 6 && memcmp(out, "\xe6\xb1\x89\xe5\xad\x97", 6) == 0, "UTF-8 中文");

        /* URL-safe 字符 */
        n = b64_decode("-_8", 3, out, sizeof(out));
        check(n == 2, "兼容 URL-safe 字母表 (- _)");

        n = b64_decode("Zm9v!!!", 7, out, sizeof(out));
        check(n == (size_t)-1, "非法字符被拒绝");
    }
}

static void test_sha256(void)
{
    char hex[65];

    printf("[5] SHA-256 标准测试向量\n");
    sha256_text("", 0, hex, 0);
    check_str(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              "空字符串");
    sha256_text("abc", 3, hex, 0);
    check_str(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "\"abc\"");
    sha256_text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hex, 0);
    check_str(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
              "448bit 官方向量");

    /* 55 / 56 / 57 字节跨越补位边界 */
    printf("[6] SHA-256 补位边界（55 / 56 / 57 / 63 / 64 / 65 字节）\n");
    {
        struct { size_t n; const char *want; } c[] = {
            { 55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318" },
            { 56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a" },
            { 57, "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6" },
            { 63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34" },
            { 64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb" },
            { 65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0" },
        };
        size_t i;
        for (i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
            char *buf = (char *)malloc(c[i].n + 1);
            memset(buf, 'a', c[i].n);
            buf[c[i].n] = '\0';
            sha256_text(buf, c[i].n, hex, 0);
            check_str(hex, c[i].want, "连续 'a' 的摘要");
            free(buf);
        }
    }

    /* 一百万字节 */
    printf("[7] SHA-256 百万字节流式处理\n");
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
        check_str(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                  "100 万个 'a'");
    }

    /* 分块喂入应与一次性结果一致 */
    printf("[8] SHA-256 分段喂入一致性\n");
    {
        size_t n = 123457;
        unsigned char *raw = make_data(n, 2024);
        char one[65], chunk[65];

        sha256_text(raw, n, one, 0);
        sha256_text(raw, n, chunk, 1);   /* 顺便验证大写 */
        check(strcmp(one, chunk) != 0, "大写/小写输出不同");
        check(strlen(chunk) == 64, "十六进制长度 64");

        {
            sha256_ctx ctx;
            unsigned char digest[32];
            size_t off = 0;
            static const size_t steps[] = { 1, 2, 3, 63, 64, 65, 1000, 4096, 65536 };
            size_t k = 0;
            char hex2[65];
            sha256_init(&ctx);
            while (off < n) {
                size_t step = steps[k % (sizeof(steps) / sizeof(steps[0]))];
                if (step > n - off) step = n - off;
                sha256_update(&ctx, raw + off, step);
                off += step;
                k++;
            }
            sha256_final(&ctx, digest);
            sha256_hex(digest, hex2, 0);
            check_str(hex2, one, "不规则分块结果一致");
        }
        free(raw);
    }
}

static void test_filetype(void)
{
    printf("[9] 文件类型识别\n");
    {
        struct { const unsigned char *sig; size_t n; const char *want; } c[] = {
            { (const unsigned char *)"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR", 16, ".png" },
            { (const unsigned char *)"\xff\xd8\xff\xe0\x00\x10JFIF", 10, ".jpg" },
            { (const unsigned char *)"GIF89a\x01\x00\x01\x00", 10, ".gif" },
            { (const unsigned char *)"BM\x36\x00\x00\x00\x00\x00", 10, ".bmp" },
            { (const unsigned char *)"RIFF\x24\x00\x00\x00WEBPVP8 ", 16, ".webp" },
            { (const unsigned char *)"%PDF-1.7\n%\xe2\xe3\xcf\xd3", 14, ".pdf" },
            { (const unsigned char *)"PK\x03\x04\x14\x00\x00\x00\x08\x00", 12, ".zip" },
            { (const unsigned char *)"Rar!\x1a\x07\x00\xcf\x90", 9, ".rar" },
            { (const unsigned char *)"7z\xbc\xaf\x27\x1c\x00\x04", 8, ".7z" },
            { (const unsigned char *)"\x1f\x8b\x08\x00\x00\x00", 6, ".gz" },
            { (const unsigned char *)"\x00\x00\x01\x00\x01\x00\x20\x20", 8, ".ico" },
            { (const unsigned char *)"hello world\nthis is text\n", 25, ".txt" },
            { (const unsigned char *)"\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09", 10, ".bin" },
        };
        size_t i;
        for (i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
            const char *desc = NULL;
            const char *ext = detect_file_ext(c[i].sig, c[i].n, &desc);
            check_str(ext, c[i].want, desc ? desc : "?");
        }
    }
}

static void test_vectors_file(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[262144];
    int lineno = 0, bad = 0, cnt = 0;

    printf("[10] 与 Python 标准库随机对拍（%s）\n", path);
    if (!fp) {
        printf("  [WARN] 打不开对拍文件，跳过\n");
        return;
    }
    while (fgets(line, sizeof(line), fp)) {
        char *hexin, *wantb64, *wantsha;
        size_t hexlen, n, i;
        unsigned char *raw;
        char *enc, got[65];

        lineno++;
        if (line[0] == '#' || line[0] == '\n')
            continue;

        hexin = strtok(line, " \t\r\n");
        wantb64 = strtok(NULL, " \t\r\n");
        wantsha = strtok(NULL, " \t\r\n");
        if (!hexin || !wantb64 || !wantsha) {
            printf("  [WARN] 第 %d 行格式异常，跳过\n", lineno);
            continue;
        }

        hexlen = strlen(hexin);
        /* 空样本用 "-" 表示 */
        n = (strcmp(hexin, "-") == 0) ? 0 : hexlen / 2;
        raw = (unsigned char *)malloc(n ? n : 1);
        for (i = 0; i < n; i++) {
            char b[3] = { hexin[i * 2], hexin[i * 2 + 1], 0 };
            raw[i] = (unsigned char)strtoul(b, NULL, 16);
        }
        if (strcmp(wantb64, "-") == 0)
            wantb64 = "";

        enc = (char *)malloc(b64_encoded_len(n) + 1);
        b64_encode(raw, n, enc);
        if (strcmp(enc, wantb64) != 0) {
            g_fail++; bad++;
            printf("  [FAIL] n=%zu Base64 不一致\n    got=%s\n    want=%s\n", n, enc, wantb64);
        }

        sha256_text(raw, n, got, 0);
        if (strcmp(got, wantsha) != 0) {
            g_fail++; bad++;
            printf("  [FAIL] n=%zu SHA256 不一致\n    got=%s\n    want=%s\n", n, got, wantsha);
        } else {
            /* 解码回原样 */
            unsigned char *dec = (unsigned char *)malloc(n + 16);
            size_t dlen = b64_decode(wantb64, strlen(wantb64), dec, n + 16);
            if (dlen != n || (n && memcmp(dec, raw, n) != 0)) {
                g_fail++; bad++;
                printf("  [FAIL] n=%zu 解码未还原\n", n);
            }
            free(dec);
        }
        free(raw); free(enc);
        cnt++; g_total++;
    }
    fclose(fp);
    printf("  对拍样本 %d 条，异常 %d 条\n", cnt, bad);
}

int main(int argc, char **argv)
{
    printf("==== b64sha 核心算法自检 ====\n");
    test_base64_vectors();
    test_sha256();
    test_filetype();
    if (argc > 1)
        test_vectors_file(argv[1]);

    printf("\n==== 小结：断言 %d 项，失败 %d 项 ====\n", g_total, g_fail);
    if (g_fail == 0)
        printf("结果：ALL PASS\n");
    else
        printf("结果：FAILED\n");
    return g_fail == 0 ? 0 : 1;
}
