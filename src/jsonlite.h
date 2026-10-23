/*
 * jsonlite.h - 容错 JSON 扫描器（专为"解析接口响应"这个窄用途设计）
 *
 * 为什么不用完整 parser：
 *   我们只需要三件事——取 errorCode / errorMsg、找出响应里所有图片 Base64、生成脱敏副本。
 *   完整 parser 需要建树、分配节点、处理所有 JSON 语法边界，代码量和出错面都大得多。
 *   这里采用"单遍、不建树、只记偏移"的容错扫描：任何不认识的字节只前进一字节，
 *   遇到截断或非 JSON 内容也不会崩，最坏情况是"什么都没提取到"。
 *
 * 本模块刻意不依赖任何操作系统 API，因此可在 macOS / Linux 上原生编译并单测
 * （见 tests/test_jsonlite.c），同一份源码再参与 Windows 构建。
 */
#ifndef JSONLITE_H
#define JSONLITE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JL_MAX_KEYS   8
#define JL_PATH_MAX   192

/* ------------------------------------------------------------------
 * 图片引用：只记录该 Base64 在原 JSON 中的位置，不复制数据
 * ------------------------------------------------------------------ */
typedef struct {
    size_t val_off;              /* 字符串内容起点（左引号之后） */
    size_t val_len;              /* 内容长度（不含两侧引号） */
    size_t prefix_len;           /* "data:image/png;base64," 的长度；0 表示无前缀 */
    char   path[JL_PATH_MAX];    /* 人类可读路径，如 result.layoutParsingResults[0].outputImages.layout_det_res */
    char   ext[8];               /* ".png" / ".jpg" / ".tif" ...（以魔数为准） */
} jl_image_ref;

typedef struct {
    jl_image_ref *items;
    size_t        count;
    size_t        cap;
} jl_image_list;

void jl_image_list_free(jl_image_list *l);

/* ------------------------------------------------------------------
 * 字段命中结果
 * ------------------------------------------------------------------ */
typedef struct {
    const char *key;
    int         is_string;   /* 1 = 字符串（off/len 为内容区间）；0 = 标量（原始 token 区间） */
    size_t      off;
    size_t      len;
    int         found;
} jl_field;

typedef struct {
    /*
     * 指向被扫描的原始 JSON 文本。summary 只在调用方持有该缓冲期间有效——
     * 字段值以"偏移 + 长度"记录，取值时才从原文切片，因此扫描阶段零拷贝。
     */
    const char   *json;
    size_t        len;
    jl_field      fields[JL_MAX_KEYS];
    int           nfields;
    jl_image_list images;
    size_t        images_b64_chars;   /* 所有图片 Base64 字符数合计（给界面显示"图片多大"） */
} jl_summary;

void jl_summary_free(jl_summary *s);

/* ------------------------------------------------------------------
 * 主扫描
 *
 * 单遍遍历，同时：
 *   · 记录 keys 中每个 key 首次出现的值（只记最外层第一次，避免被内层同名 key 覆盖）
 *   · 收集所有被判定为图片的 Base64 字符串
 *
 * 返回 0 表示扫描完成。JSON 损坏/截断时同样返回 0，只是可能提取不全。
 * ------------------------------------------------------------------ */
int jl_scan(const char *json, size_t len,
            const char *const *keys, int nkeys,
            jl_summary *out);

/* 从 summary 取值 */
int jl_field_int(const jl_summary *s, const char *key, long *out);
int jl_field_str(const jl_summary *s, const char *key, char *out, size_t cap);

/* 便捷封装：单 key 直接查（内部走 JL_scan，适合测试与小规模调用） */
int jl_find_int(const char *json, size_t len, const char *key, long *out);
int jl_find_str(const char *json, size_t len, const char *key, char *out, size_t cap);
int jl_scan_images(const char *json, size_t len, jl_image_list *out);

/* ------------------------------------------------------------------
 * 脱敏 / 预览
 * ------------------------------------------------------------------ */

/*
 * 生成脱敏副本：把所有命中图片的字符串值整体替换为 "...", 其余字节原样保留。
 * 输出仍是合法 JSON（若输入合法），以 '\0' 结尾，需 free。
 */
int jl_redact(const char *json, size_t len, const jl_image_list *imgs,
              char **out, size_t *out_len);

/*
 * 预览截断：不在字符串内部、优先在 ',' 处断开，避免把 JSON 切成半截字符串。
 * 输出以 '\0' 结尾，需 free。truncated 非空时回写是否发生了截断。
 */
int jl_truncate_preview(const char *src, size_t len, size_t cap,
                        char **out, size_t *out_len, int *truncated);

/* 首个非空白字符是否为 '{' 或 '[' */
int jl_is_json(const char *s, size_t len);

/* JSON 字符串反转义（\" \\ \/ \b \f \n \r \t \uXXXX）。需 free。 */
int jl_unescape(const char *s, size_t n, char **out, size_t *out_len);

/* 取图片的真实 Base64 指针（已跳过 data: 前缀），不复制、不修改 */
const char *jl_image_payload_ptr(const char *json, const jl_image_ref *ref,
                                 size_t *out_len);

/* 内置自检：把报告追加写入 report（UTF-8），返回失败项数 */
int jl_selftest(char *report, size_t cch);

#ifdef __cplusplus
}
#endif

#endif /* JSONLITE_H */
