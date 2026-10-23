/*
 * b64sha.h - 纯 C 实现的 Base64 编解码 / SHA-256 / 文件类型识别
 *
 * 本模块刻意不依赖任何操作系统 API，因此：
 *   1) 可在 macOS / Linux 上用本机 gcc 原生编译并跑测试向量（见 tests/test_core.c）；
 *   2) 同一份源码直接参与 Windows 目标的可执行文件构建。
 * 这样保证"在 Windows 上跑的那份算法"就是"已验证过的那份算法"。
 */
#ifndef B64SHA_H
#define B64SHA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ Base64 ============================ */

/* 编码后字符串长度（不含结尾 '\0'）。n 为原始字节数。 */
size_t b64_encoded_len(size_t n);

/* 编码。out 容量必须 >= b64_encoded_len(n) + 1，返回 0 表示成功。 */
int b64_encode(const unsigned char *in, size_t n, char *out);

/*
 * 解码。宽松模式：
 *   - 自动忽略空白字符（空格、制表、换行、回车）与 '=' 填充；
 *   - 同时兼容标准字母表（+ /）与 URL-safe 字母表（- _）；
 *   - 遇到其它非法字符返回 (size_t)-1。
 * 返回值：解码出的字节数；out 容量不足同样返回 (size_t)-1。
 */
size_t b64_decode(const char *in, size_t inlen, unsigned char *out, size_t outcap);

/* 解码所需的最大输出缓冲容量（按输入长度估算，含余量）。 */
size_t b64_decoded_cap(size_t inlen);

/* ============================ SHA-256 ============================ */

typedef struct {
    uint32_t      h[8];
    uint64_t      total;      /* 已处理字节数 */
    unsigned char block[64];  /* 未满一块的残留数据 */
    size_t        block_len;
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const void *data, size_t len);
void sha256_final(sha256_ctx *ctx, unsigned char digest[32]);

/* 一次性计算并转成十六进制字符串。hex 至少 65 字节。uppercase 非 0 时输出大写。 */
void sha256_text(const void *data, size_t len, char *hex, int uppercase);

/* 32 字节摘要 -> 十六进制字符串（hex 至少 65 字节）。 */
void sha256_hex(const unsigned char digest[32], char *hex, int uppercase);

/* ======================= 文件类型识别 ======================= */

/*
 * 通过文件头魔数 + 轻量启发式判断文件类型。
 * 返回扩展名（形如 ".png"，全部小写），无法判断时返回 ".bin"。
 * desc 非空时，写入中文类型描述（如 "PNG 图片"）。
 */
const char *detect_file_ext(const unsigned char *data, size_t n, const char **desc);

/*
 * 可选的"目标文件类型"下拉框选项。索引 0 约定为"自动判断"。
 * 返回值为扩展名（无点，如 "png"）；"自动判断"项返回 ""。
 */
size_t      filetype_option_count(void);
const char *filetype_option_name(size_t index);  /* 显示名，如 "PNG 图片 (*.png)" */
const char *filetype_option_ext(size_t index);   /* 扩展名，如 "png"；自动判断为 "" */

#ifdef __cplusplus
}
#endif

#endif /* B64SHA_H */
