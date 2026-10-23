/*
 * b64sha.c - 纯 C 实现的 Base64 编解码 / SHA-256 / 文件类型识别
 * 只依赖 <string.h> <stdlib.h>，不调用任何平台 API。
 */
#include "b64sha.h"
#include <string.h>

/* =====================================================================
 * Base64
 * ===================================================================== */

static const char B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64_encoded_len(size_t n)
{
    return ((n + 2) / 3) * 4;
}

int b64_encode(const unsigned char *in, size_t n, char *out)
{
    size_t i = 0, o = 0;

    while (i + 3 <= n) {
        unsigned int v = ((unsigned int)in[i] << 16) |
                         ((unsigned int)in[i + 1] << 8) |
                         (unsigned int)in[i + 2];
        out[o++] = B64_ALPHABET[(v >> 18) & 0x3F];
        out[o++] = B64_ALPHABET[(v >> 12) & 0x3F];
        out[o++] = B64_ALPHABET[(v >> 6) & 0x3F];
        out[o++] = B64_ALPHABET[v & 0x3F];
        i += 3;
    }

    if (n - i == 1) {
        unsigned int v = (unsigned int)in[i] << 16;
        out[o++] = B64_ALPHABET[(v >> 18) & 0x3F];
        out[o++] = B64_ALPHABET[(v >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (n - i == 2) {
        unsigned int v = ((unsigned int)in[i] << 16) | ((unsigned int)in[i + 1] << 8);
        out[o++] = B64_ALPHABET[(v >> 18) & 0x3F];
        out[o++] = B64_ALPHABET[(v >> 12) & 0x3F];
        out[o++] = B64_ALPHABET[(v >> 6) & 0x3F];
        out[o++] = '=';
    }

    out[o] = '\0';
    return 0;
}

static int b64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;   /* 兼容 URL-safe */
    if (c == '/' || c == '_') return 63;
    return -1;
}

static int b64_is_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '\f' || c == '\v' || c == 0xA0;
}

size_t b64_decoded_cap(size_t inlen)
{
    /* 每 4 个字符产出 3 字节，再放宽余量 */
    return (inlen / 4) * 3 + 16;
}

size_t b64_decode(const char *in, size_t inlen, unsigned char *out, size_t outcap)
{
    unsigned int acc = 0;
    int nbits = 0;
    size_t i, o = 0;

    for (i = 0; i < inlen; i++) {
        unsigned char c = (unsigned char)in[i];
        int v;

        if (b64_is_space(c) || c == '=')
            continue;

        v = b64_value(c);
        if (v < 0)
            return (size_t)-1;      /* 含非法字符 */

        acc = (acc << 6) | (unsigned int)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            if (o >= outcap)
                return (size_t)-1;
            out[o++] = (unsigned char)((acc >> nbits) & 0xFF);
        }
    }
    return o;
}

/* =====================================================================
 * SHA-256
 * ===================================================================== */

static const uint32_t SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_transform(uint32_t state[8], const unsigned char block[64])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROTR32(w[i - 15], 7) ^ ROTR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR32(w[i - 2], 17) ^ ROTR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0; i < 64; i++) {
        uint32_t S1 = ROTR32(e, 6) ^ ROTR32(e, 11) ^ ROTR32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + SHA256_K[i] + w[i];
        uint32_t S0 = ROTR32(a, 2) ^ ROTR32(a, 13) ^ ROTR32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256_init(sha256_ctx *ctx)
{
    ctx->h[0] = 0x6a09e667u; ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u; ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu; ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu; ctx->h[7] = 0x5be0cd19u;
    ctx->total = 0;
    ctx->block_len = 0;
}

void sha256_update(sha256_ctx *ctx, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;

    ctx->total += (uint64_t)len;

    if (ctx->block_len) {
        size_t need = 64 - ctx->block_len;
        if (len < need) {
            memcpy(ctx->block + ctx->block_len, p, len);
            ctx->block_len += len;
            return;
        }
        memcpy(ctx->block + ctx->block_len, p, need);
        sha256_transform(ctx->h, ctx->block);
        p += need;
        len -= need;
        ctx->block_len = 0;
    }

    while (len >= 64) {
        sha256_transform(ctx->h, p);
        p += 64;
        len -= 64;
    }

    if (len) {
        memcpy(ctx->block, p, len);
        ctx->block_len = len;
    }
}

void sha256_final(sha256_ctx *ctx, unsigned char digest[32])
{
    uint64_t bits = ctx->total * 8;
    size_t rem = (size_t)((ctx->total + 1) % 64);
    size_t pad_len = (rem <= 56) ? (57 - rem) : (121 - rem);  /* 含 0x80，最大 64 */
    unsigned char pad[72];
    int i;

    pad[0] = 0x80;
    memset(pad + 1, 0, pad_len - 1);

    /* 末尾追加 64bit 大端比特长度，pad_len + 8 <= 72 */
    for (i = 7; i >= 0; i--)
        pad[pad_len + (7 - i)] = (unsigned char)((bits >> (i * 8)) & 0xFF);

    sha256_update(ctx, pad, pad_len + 8);

    for (i = 0; i < 8; i++) {
        digest[i * 4]     = (unsigned char)((ctx->h[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = (unsigned char)((ctx->h[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = (unsigned char)((ctx->h[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = (unsigned char)(ctx->h[i] & 0xFF);
    }
}

void sha256_hex(const unsigned char digest[32], char *hex, int uppercase)
{
    static const char *lower = "0123456789abcdef";
    static const char *upper = "0123456789ABCDEF";
    const char *tbl = uppercase ? upper : lower;
    int i;

    for (i = 0; i < 32; i++) {
        hex[i * 2]     = tbl[(digest[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = tbl[digest[i] & 0x0F];
    }
    hex[64] = '\0';
}

void sha256_text(const void *data, size_t len, char *hex, int uppercase)
{
    sha256_ctx ctx;
    unsigned char digest[32];

    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);
    sha256_hex(digest, hex, uppercase);
}

/* =====================================================================
 * 文件类型识别
 * ===================================================================== */

#define NEED(n)  (len >= (n))

static int starts_with(const unsigned char *d, size_t len, const char *sig, size_t siglen)
{
    return len >= siglen && memcmp(d, sig, siglen) == 0;
}

static int contains_in(const unsigned char *d, size_t len, size_t window,
                       const char *needle, size_t nlen)
{
    size_t limit = len < window ? len : window;
    size_t i;

    if (limit < nlen)
        return 0;
    for (i = 0; i + nlen <= limit; i++) {
        if (memcmp(d + i, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

/* 文本文件判定：前若干字节里可打印字符占比高且不含 NUL */
static int looks_like_text(const unsigned char *d, size_t len)
{
    size_t probe = len < 4096 ? len : 4096;
    size_t i, printable = 0;

    if (probe == 0)
        return 0;

    for (i = 0; i < probe; i++) {
        unsigned char c = d[i];
        if (c == 0)
            return 0;                                  /* 含 NUL，视为二进制 */
        if ((c >= 0x20 && c <= 0x7E) || c == '\t' || c == '\n' || c == '\r' || c >= 0x80)
            printable++;
    }
    return printable * 100 / probe >= 98;
}

const char *detect_file_ext(const unsigned char *data, size_t len, const char **desc)
{
    const char *ext = NULL;
    const char *name = NULL;

    if (data == NULL || len < 4) {
        if (desc) *desc = "未知二进制数据";
        return ".bin";
    }

    if (starts_with(data, len, "\x89PNG\r\n\x1a\n", 8)) {
        ext = ".png"; name = "PNG 图片";
    } else if (NEED(3) && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        ext = ".jpg"; name = "JPEG 图片";
    } else if (starts_with(data, len, "GIF87a", 6) || starts_with(data, len, "GIF89a", 6)) {
        ext = ".gif"; name = "GIF 图片";
    } else if (NEED(2) && data[0] == 'B' && data[1] == 'M') {
        ext = ".bmp"; name = "BMP 图片";
    } else if (starts_with(data, len, "RIFF", 4) && len >= 12 &&
               memcmp(data + 8, "WEBP", 4) == 0) {
        ext = ".webp"; name = "WebP 图片";
    } else if (starts_with(data, len, "RIFF", 4) && len >= 12 &&
               memcmp(data + 8, "WAVE", 4) == 0) {
        ext = ".wav"; name = "WAV 音频";
    } else if (starts_with(data, len, "%PDF", 4)) {
        ext = ".pdf"; name = "PDF 文档";
    } else if (starts_with(data, len, "PK\x03\x04", 4) ||
               starts_with(data, len, "PK\x05\x06", 4) ||
               starts_with(data, len, "PK\x07\x08", 4)) {
        /* OOXML 全家桶也是 zip 容器，靠内部条目名区分 */
        if (contains_in(data, len, 8192, "[Content_Types].xml", 20) ||
            contains_in(data, len, 8192, "word/", 5) ||
            contains_in(data, len, 8192, "xl/", 3) ||
            contains_in(data, len, 8192, "ppt/", 4)) {
            if (contains_in(data, len, 8192, "word/", 5)) {
                ext = ".docx"; name = "Word 文档";
            } else if (contains_in(data, len, 8192, "xl/", 3)) {
                ext = ".xlsx"; name = "Excel 工作簿";
            } else if (contains_in(data, len, 8192, "ppt/", 4)) {
                ext = ".pptx"; name = "PowerPoint 演示文稿";
            } else {
                ext = ".zip"; name = "ZIP 压缩包";
            }
        } else {
            ext = ".zip"; name = "ZIP 压缩包";
        }
    } else if (starts_with(data, len, "Rar!\x1a\x07", 6)) {
        ext = ".rar"; name = "RAR 压缩包";
    } else if (starts_with(data, len, "7z\xbc\xaf\x27\x1c", 6)) {
        ext = ".7z"; name = "7-Zip 压缩包";
    } else if (NEED(2) && data[0] == 0x1F && data[1] == 0x8B) {
        ext = ".gz"; name = "GZIP 压缩包";
    } else if (starts_with(data, len, "ID3", 3) ||
               (NEED(2) && data[0] == 0xFF && (data[1] & 0xE0) == 0xE0)) {
        ext = ".mp3"; name = "MP3 音频";
    } else if (NEED(4) && data[0] == 0x00 && data[1] == 0x00 &&
               data[2] == 0x01 && data[3] == 0x00) {
        ext = ".ico"; name = "ICO 图标";
    } else if (starts_with(data, len, "II\x2a\x00", 4) || starts_with(data, len, "MM\x00\x2a", 4)) {
        ext = ".tif"; name = "TIFF 图片";
    } else if (NEED(8) && memcmp(data + 4, "ftyp", 4) == 0) {
        ext = ".mp4"; name = "MP4 视频";
    } else if (starts_with(data, len, "OTTO", 4) ||
               (NEED(4) && data[0] == 0x00 && data[1] == 0x01 &&
                data[2] == 0x00 && data[3] == 0x00)) {
        ext = ".ttf"; name = "字体文件";
    } else if (starts_with(data, len, "<?xml", 5)) {
        if (contains_in(data, len, 4096, "WordDocument", 12)) {
            ext = ".doc"; name = "Word 文档 (旧版)";
        } else if (contains_in(data, len, 4096, "Workbook", 8)) {
            ext = ".xls"; name = "Excel 工作簿 (旧版)";
        } else {
            ext = ".xml"; name = "XML 文档";
        }
    } else if (starts_with(data, len, "\xef\xbb\xbf", 3)) {
        ext = ".txt"; name = "文本文件 (UTF-8 BOM)";
    } else if (looks_like_text(data, len)) {
        ext = ".txt"; name = "文本文件";
    } else {
        ext = ".bin"; name = "未知二进制数据";
    }

    if (desc)
        *desc = name;
    return ext;
}

/* ======================= 下拉框选项表 ======================= */

typedef struct {
    const char *name;   /* 显示名 */
    const char *ext;    /* 扩展名，无点；自动判断为空串 */
} filetype_option;

static const filetype_option FILETYPE_OPTIONS[] = {
    { "自动判断（按文件内容）",   ""     },
    { "PNG 图片",                "png"  },
    { "JPEG 图片",               "jpg"  },
    { "GIF 图片",                "gif"  },
    { "BMP 图片",                "bmp"  },
    { "WebP 图片",               "webp" },
    { "TIFF 图片",               "tif"  },
    { "ICO 图标",                "ico"  },
    { "PDF 文档",                "pdf"  },
    { "文本文件 TXT",            "txt"  },
    { "JSON 文件",               "json" },
    { "XML 文件",                "xml"  },
    { "HTML 文件",               "html" },
    { "CSV 文件",                "csv"  },
    { "ZIP 压缩包",              "zip"  },
    { "RAR 压缩包",              "rar"  },
    { "7-Zip 压缩包",            "7z"   },
    { "GZIP 压缩包",             "gz"   },
    { "Word 文档 DOCX",          "docx" },
    { "Excel 工作簿 XLSX",       "xlsx" },
    { "PowerPoint PPTX",         "pptx" },
    { "二进制文件 BIN",          "bin"  },
};

size_t filetype_option_count(void)
{
    return sizeof(FILETYPE_OPTIONS) / sizeof(FILETYPE_OPTIONS[0]);
}

const char *filetype_option_name(size_t index)
{
    if (index >= filetype_option_count())
        return NULL;
    return FILETYPE_OPTIONS[index].name;
}

const char *filetype_option_ext(size_t index)
{
    if (index >= filetype_option_count())
        return NULL;
    return FILETYPE_OPTIONS[index].ext;
}
