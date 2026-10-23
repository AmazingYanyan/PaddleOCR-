/*
 * jsonlite.c - 容错 JSON 扫描器
 *
 * 设计要点（与完整 parser 的取舍）：
 *   1) 不建树、不分配节点，只记录"值在原文中的偏移"。这样即使响应有几十 MB，
 *      内存开销也只有几条记录；取值时才从原文切片。
 *   2) 容错优先：遇到不认识的字节只前进一字节继续扫，遇到截断的 JSON 也只是
 *      "少提取一些"，绝不崩、不死循环。所有循环都有"必须前进"的保证。
 *   3) 递归深度上限 64，超过后改为"跳过平衡容器"，不会因为深嵌套而爆栈。
 *   4) 字符串里的转义按 '\\' 成对处理；UTF-8 多字节不会误判——因为 JSON 的
 *      结构字符都是 ASCII（<0x80），而 UTF-8 续字节都 >=0x80。
 */
#include "jsonlite.h"
#include "b64sha.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JL_MAX_DEPTH   64
#define JL_IMG_MIN_LEN 256   /* 小于此长度的字符串不可能是图片 Base64 */

/* ==================================================================
 * 报告辅助（自检用，风格与 selftest.c 保持一致）
 * ================================================================== */
static void jl_rep(char *out, size_t cch, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (!out || *used >= cch - 1)
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

/* ==================================================================
 * 基础工具
 * ================================================================== */

static int jl_ws(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static int jl_b64_char(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/' ||
           c == '=' || c == '-' || c == '_';
}

static int jl_is_image_ext(const char *ext)
{
    static const char *ok[] = {
        ".png", ".jpg", ".gif", ".bmp", ".webp", ".tif", ".ico"
    };
    size_t i;

    if (!ext)
        return 0;
    for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        if (strcmp(ext, ok[i]) == 0)
            return 1;
    }
    return 0;
}

/* 从 data:image/<mime>;base64, 前缀里取扩展名 */
static const char *jl_mime_ext(const char *s, size_t pre)
{
    char mime[32];
    size_t i, m = 0;

    if (!s || pre < 12)
        return NULL;
    for (i = 11; i < pre && m + 1 < sizeof(mime); i++) {
        if (s[i] == ';' || s[i] == ',')
            break;
        mime[m++] = s[i];
    }
    mime[m] = '\0';

    if (strcmp(mime, "png") == 0)     return ".png";
    if (strcmp(mime, "jpeg") == 0)    return ".jpg";
    if (strcmp(mime, "jpg") == 0)     return ".jpg";
    if (strcmp(mime, "gif") == 0)     return ".gif";
    if (strcmp(mime, "bmp") == 0)     return ".bmp";
    if (strcmp(mime, "webp") == 0)    return ".webp";
    if (strcmp(mime, "tiff") == 0)    return ".tif";
    if (strcmp(mime, "tif") == 0)     return ".tif";
    if (strcmp(mime, "x-icon") == 0)  return ".ico";
    return NULL;
}

/* 若字符串以 data:image/...;base64, 开头，返回前缀长度；否则 0 */
static size_t jl_data_prefix(const char *s, size_t off, size_t len)
{
    size_t i;

    if (len < 22)
        return 0;
    if (memcmp(s + off, "data:image/", 11) != 0)
        return 0;
    for (i = 11; i < len && i < 96; i++) {
        if (s[off + i] == ',') {
            if (i >= 7 && memcmp(s + off + i - 7, ";base64", 7) == 0)
                return i + 1;
            return 0;
        }
    }
    return 0;
}

/* ==================================================================
 * 扫描器
 * ================================================================== */

typedef struct {
    const char *s;
    size_t      n;
    size_t      i;

    int         depth;
    char        path[JL_PATH_MAX];
    size_t      path_len;

    /* 当前值的 key（仅直接子节点有效），用于把值归属到待抓取字段 */
    const char *cur_key;
    size_t      cur_key_len;

    jl_field    fields[JL_MAX_KEYS];
    int         nfields;

    jl_image_list *imgs;
    size_t       b64_chars;
} scan_ctx;

static void skip_ws(scan_ctx *c)
{
    while (c->i < c->n) {
        unsigned char ch = (unsigned char)c->s[c->i];
        if (jl_ws(ch)) {
            c->i++;
            continue;
        }
        /* 只在整个文档最开头跳过 UTF-8 BOM */
        if (c->i == 0 && c->n >= 3 && ch == 0xEF &&
            (unsigned char)c->s[1] == 0xBB && (unsigned char)c->s[2] == 0xBF) {
            c->i = 3;
            continue;
        }
        break;
    }
}

static void path_append(scan_ctx *c, const char *seg)
{
    size_t l, seplen;
    const char *sep;

    if (!seg)
        return;
    l = strlen(seg);
    if (l == 0)
        return;
    sep = (c->path_len > 0 && seg[0] != '[') ? "." : "";
    seplen = strlen(sep);
    if (c->path_len + seplen + l >= JL_PATH_MAX)
        return;                        /* 超长就不再追加，保留已有前缀 */
    memcpy(c->path + c->path_len, sep, seplen);
    c->path_len += seplen;
    memcpy(c->path + c->path_len, seg, l);
    c->path_len += l;
    c->path[c->path_len] = '\0';
}

/* 读一个字符串 token；c->i 必须指向左引号。off/len 为内容区间（不含引号） */
static void scan_string(scan_ctx *c, size_t *off, size_t *len)
{
    size_t start;

    *off = 0;
    *len = 0;
    if (c->i >= c->n || c->s[c->i] != '"')
        return;
    c->i++;
    start = c->i;
    while (c->i < c->n) {
        char ch = c->s[c->i];
        if (ch == '\\') {
            c->i += 2;                 /* 跳过转义对；越界由 while 条件兜住 */
            continue;
        }
        if (ch == '"') {
            *off = start;
            *len = c->i - start;
            c->i++;
            return;
        }
        c->i++;
    }
    /* 未闭合：把剩余内容全部当作字符串 */
    *off = start;
    *len = c->n - start;
}

/* 深度超限时，不递归、只把整个平衡容器吃掉 */
static void skip_container(scan_ctx *c)
{
    int level = 0;

    while (c->i < c->n) {
        char ch = c->s[c->i];
        if (ch == '"') {
            size_t o, l;
            scan_string(c, &o, &l);
            continue;
        }
        if (ch == '{' || ch == '[') {
            level++;
            c->i++;
            continue;
        }
        if (ch == '}' || ch == ']') {
            level--;
            c->i++;
            if (level <= 0)
                return;
            continue;
        }
        c->i++;
    }
}

static void note_value(scan_ctx *c, size_t off, size_t len, int is_string)
{
    int k;

    if (!c->cur_key || c->cur_key_len == 0)
        return;
    for (k = 0; k < c->nfields; k++) {
        jl_field *f = &c->fields[k];
        if (f->found || !f->key)
            continue;
        if (strlen(f->key) != c->cur_key_len)
            continue;
        if (memcmp(f->key, c->cur_key, c->cur_key_len) != 0)
            continue;
        f->found = 1;
        f->is_string = is_string;
        f->off = off;
        f->len = len;
        return;
    }
}

static void append_image(scan_ctx *c, size_t off, size_t len,
                         size_t pre, const char *ext)
{
    jl_image_list *l = c->imgs;
    jl_image_ref *ref;

    if (!l)
        return;
    if (l->count >= l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 8;
        jl_image_ref *ni = (jl_image_ref *)realloc(l->items, ncap * sizeof(*ni));
        if (!ni)
            return;
        l->items = ni;
        l->cap = ncap;
    }
    ref = &l->items[l->count];
    memset(ref, 0, sizeof(*ref));
    ref->val_off = off;
    ref->val_len = len;
    ref->prefix_len = pre;
    snprintf(ref->path, sizeof(ref->path), "%s", c->path[0] ? c->path : "(root)");
    snprintf(ref->ext, sizeof(ref->ext), "%s", ext ? ext : ".bin");
    l->count++;
    c->b64_chars += len - pre;
}

/*
 * 图片判定（四重条件，缺一不可）——本模块最容易出错的地方：
 *   1) 长度 >= 256
 *   2) 去掉可选 data URL 前缀后，字符全部落在 Base64 字母表内
 *   3) 长度是 4 的倍数
 *   4) 解出前 48 字节后，魔数必须命中图片类型
 * 只靠"键名叫 inputImage"或"字符串很长"都会误判 Markdown 正文，
 * 所以这里必须真的解一段出来看魔数。
 */
static void maybe_image(scan_ctx *c, size_t off, size_t len)
{
    size_t pre, plen, i;
    unsigned char head[48];
    size_t got;
    const char *ext;

    if (!c->imgs || len < JL_IMG_MIN_LEN)
        return;

    pre = jl_data_prefix(c->s, off, len);
    plen = len - pre;
    if (plen < JL_IMG_MIN_LEN)
        return;
    if (plen % 4 != 0)
        return;

    for (i = 0; i < plen; i++) {
        if (!jl_b64_char((unsigned char)c->s[off + pre + i]))
            return;                    /* 含非 Base64 字符：普通文本 */
    }

    got = b64_decode(c->s + off + pre, (size_t)64, head, sizeof(head));
    if (got == (size_t)-1 || got < 8)
        return;

    ext = detect_file_ext(head, got, NULL);
    if (!jl_is_image_ext(ext)) {
        /* 魔数不认识：只有带明确 MIME 且字符串足够长时才按 MIME 接受 */
        if (pre == 0 || len < 4096)
            return;
        ext = jl_mime_ext(c->s + off, pre);
        if (!jl_is_image_ext(ext))
            return;
    }

    append_image(c, off, len, pre, ext);
}

static void parse_value(scan_ctx *c);

static void parse_object(scan_ctx *c)
{
    c->i++;                            /* '{' */

    for (;;) {
        size_t save_len, koff, klen;
        char seg[96];
        size_t n;

        skip_ws(c);
        if (c->i >= c->n)
            return;
        if (c->s[c->i] == '}') {
            c->i++;
            return;
        }
        if (c->s[c->i] != '"') {
            c->i++;                    /* 容错：跳过无法识别的字节 */
            continue;
        }

        scan_string(c, &koff, &klen);
        skip_ws(c);
        if (c->i < c->n && c->s[c->i] == ':')
            c->i++;
        /* 缺冒号也继续，尽量往下扫 */

        save_len = c->path_len;
        n = klen < sizeof(seg) - 1 ? klen : sizeof(seg) - 1;
        memcpy(seg, c->s + koff, n);
        seg[n] = '\0';
        path_append(c, seg);

        c->cur_key = c->s + koff;
        c->cur_key_len = klen;
        parse_value(c);
        c->cur_key = NULL;
        c->cur_key_len = 0;
        c->path_len = save_len;
        c->path[save_len] = '\0';

        skip_ws(c);
        if (c->i < c->n && c->s[c->i] == ',') {
            c->i++;
            continue;
        }
        if (c->i < c->n && c->s[c->i] == '}') {
            c->i++;
            return;
        }
        /* 其它字符：交给下一轮循环，parse_value 保证会前进 */
    }
}

static void parse_array(scan_ctx *c)
{
    unsigned idx = 0;

    c->i++;                            /* '[' */

    for (;;) {
        size_t save_len;
        char tag[24];

        skip_ws(c);
        if (c->i >= c->n)
            return;
        if (c->s[c->i] == ']') {
            c->i++;
            return;
        }

        save_len = c->path_len;
        snprintf(tag, sizeof(tag), "[%u]", idx);
        path_append(c, tag);

        c->cur_key = NULL;             /* 数组元素不归属到外层 key */
        c->cur_key_len = 0;
        parse_value(c);
        c->cur_key = NULL;
        c->cur_key_len = 0;
        c->path_len = save_len;
        c->path[save_len] = '\0';
        idx++;

        skip_ws(c);
        if (c->i < c->n && c->s[c->i] == ',') {
            c->i++;
            continue;
        }
        if (c->i < c->n && c->s[c->i] == ']') {
            c->i++;
            return;
        }
    }
}

static void parse_value(scan_ctx *c)
{
    skip_ws(c);
    if (c->i >= c->n)
        return;

    switch (c->s[c->i]) {
    case '{':
        if (c->depth >= JL_MAX_DEPTH) {
            skip_container(c);
            return;
        }
        c->depth++;
        parse_object(c);
        c->depth--;
        return;

    case '[':
        if (c->depth >= JL_MAX_DEPTH) {
            skip_container(c);
            return;
        }
        c->depth++;
        parse_array(c);
        c->depth--;
        return;

    case '"': {
        size_t off, len;
        scan_string(c, &off, &len);
        note_value(c, off, len, 1);
        maybe_image(c, off, len);
        return;
    }

    default: {
        /* 数字 / true / false / null：吃到一个分隔符为止 */
        size_t start = c->i;
        while (c->i < c->n) {
            unsigned char ch = (unsigned char)c->s[c->i];
            if (ch == ',' || ch == '}' || ch == ']' || jl_ws(ch))
                break;
            c->i++;
        }
        if (c->i == start)
            c->i++;                    /* 保证前进，杜绝死循环 */
        note_value(c, start, c->i - start, 0);
        return;
    }
    }
}

/* ==================================================================
 * 公共 API
 * ================================================================== */

void jl_image_list_free(jl_image_list *l)
{
    if (!l)
        return;
    free(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

void jl_summary_free(jl_summary *s)
{
    if (!s)
        return;
    jl_image_list_free(&s->images);
    memset(s, 0, sizeof(*s));
}

int jl_scan(const char *json, size_t len,
            const char *const *keys, int nkeys,
            jl_summary *out)
{
    scan_ctx c;
    int i;

    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));

    if (!json || len == 0)
        return 0;

    memset(&c, 0, sizeof(c));
    c.s = json;
    c.n = len;
    c.imgs = &out->images;

    if (nkeys < 0)
        nkeys = 0;
    if (nkeys > JL_MAX_KEYS)
        nkeys = JL_MAX_KEYS;
    c.nfields = nkeys;
    for (i = 0; i < nkeys; i++) {
        c.fields[i].key = keys ? keys[i] : NULL;
        c.fields[i].found = 0;
    }

    skip_ws(&c);
    {
        int guard = 0;
        while (c.i < c.n) {
            size_t before = c.i;
            c.cur_key = NULL;
            c.cur_key_len = 0;
            parse_value(&c);
            if (c.i == before)
                c.i++;
            skip_ws(&c);
            if (++guard > 8)
                break;                 /* 顶层异常内容，避免无意义长扫 */
        }
    }

    out->json = json;
    out->len = len;
    out->nfields = nkeys;
    for (i = 0; i < nkeys; i++)
        out->fields[i] = c.fields[i];
    out->images_b64_chars = c.b64_chars;
    return 0;
}

/* 取字段原始区间（不做任何转换） */
static const jl_field *jl_pick(const jl_summary *s, const char *key)
{
    int k;

    if (!s || !key)
        return NULL;
    for (k = 0; k < s->nfields; k++) {
        if (s->fields[k].found && s->fields[k].key &&
            strcmp(s->fields[k].key, key) == 0)
            return &s->fields[k];
    }
    return NULL;
}

int jl_field_str(const jl_summary *s, const char *key, char *out, size_t cap)
{
    const jl_field *f = jl_pick(s, key);
    char *u = NULL;
    size_t un = 0;

    if (!f || !out || cap == 0)
        return 0;
    out[0] = '\0';

    if (f->is_string) {
        if (jl_unescape(s->json + f->off, f->len, &u, &un) != 0)
            return 0;
        if (un >= cap)
            un = cap - 1;
        memcpy(out, u, un);
        out[un] = '\0';
        free(u);
        return 1;
    }

    /* 非字符串：原样切片（例如 errorCode 被写成 0 没有引号） */
    if (f->off + f->len > s->len)
        return 0;
    {
        size_t n = f->len < cap - 1 ? f->len : cap - 1;
        memcpy(out, s->json + f->off, n);
        out[n] = '\0';
    }
    return 1;
}

int jl_field_int(const jl_summary *s, const char *key, long *out)
{
    char tmp[64];

    if (jl_field_str(s, key, tmp, sizeof(tmp)) != 1)
        return 0;
    {
        char *end = NULL;
        long v = strtol(tmp, &end, 10);
        if (end == tmp)
            return 0;
        if (out)
            *out = v;
    }
    return 1;
}

int jl_find_int(const char *json, size_t len, const char *key, long *out)
{
    const char *keys[1];
    jl_summary s;
    int r;

    keys[0] = key;
    if (jl_scan(json, len, keys, 1, &s) != 0)
        return 0;
    r = jl_field_int(&s, key, out);
    jl_summary_free(&s);
    return r;
}

int jl_find_str(const char *json, size_t len, const char *key, char *out, size_t cap)
{
    const char *keys[1];
    jl_summary s;
    int r;

    keys[0] = key;
    if (jl_scan(json, len, keys, 1, &s) != 0)
        return 0;
    r = jl_field_str(&s, key, out, cap);
    jl_summary_free(&s);
    return r;
}

int jl_scan_images(const char *json, size_t len, jl_image_list *out)
{
    jl_summary s;
    int r;

    if (!out)
        return -1;
    r = jl_scan(json, len, NULL, 0, &s);
    if (r != 0)
        return r;
    *out = s.images;                   /* 移交所有权 */
    memset(&s.images, 0, sizeof(s.images));
    jl_summary_free(&s);
    return 0;
}

const char *jl_image_payload_ptr(const char *json, const jl_image_ref *ref,
                                 size_t *out_len)
{
    if (out_len)
        *out_len = 0;
    if (!json || !ref)
        return NULL;
    if (ref->prefix_len > ref->val_len)
        return NULL;
    if (out_len)
        *out_len = ref->val_len - ref->prefix_len;
    return json + ref->val_off + ref->prefix_len;
}

int jl_redact(const char *json, size_t len, const jl_image_list *imgs,
              char **out, size_t *out_len)
{
    size_t cursor = 0, o = 0, k;
    char *r;

    if (!out)
        return -1;
    *out = NULL;
    if (out_len)
        *out_len = 0;
    if (!json)
        return -1;

    r = (char *)malloc(len + 1);
    if (!r)
        return -1;

    if (imgs) {
        for (k = 0; k < imgs->count; k++) {
            const jl_image_ref *ref = &imgs->items[k];
            size_t s0, e0;

            if (ref->val_off == 0)
                continue;
            s0 = ref->val_off - 1;                       /* 左引号 */
            e0 = ref->val_off + ref->val_len + 1;        /* 右引号之后 */
            if (s0 < cursor || e0 > len || s0 >= e0)
                continue;                                /* 乱序或越界：跳过 */
            memcpy(r + o, json + cursor, s0 - cursor);
            o += s0 - cursor;
            r[o++] = '"';
            r[o++] = '.';
            r[o++] = '.';
            r[o++] = '.';
            r[o++] = '"';
            cursor = e0;
        }
    }

    memcpy(r + o, json + cursor, len - cursor);
    o += len - cursor;
    r[o] = '\0';
    *out = r;
    if (out_len)
        *out_len = o;
    return 0;
}

int jl_truncate_preview(const char *src, size_t len, size_t cap,
                        char **out, size_t *out_len, int *truncated)
{
    size_t i, in_str = 0, esc = 0, cut;
    size_t last_comma = (size_t)-1;
    char *r;

    if (!out)
        return -1;
    *out = NULL;
    if (out_len)
        *out_len = 0;
    if (truncated)
        *truncated = 0;
    if (!src)
        return -1;

    if (len <= cap || cap == 0) {
        r = (char *)malloc(len + 1);
        if (!r)
            return -1;
        memcpy(r, src, len);
        r[len] = '\0';
        *out = r;
        if (out_len)
            *out_len = len;
        return 0;
    }

    /* 在 cap 之内找最后一个"不在字符串里"的逗号，避免把 JSON 切成半截字符串 */
    for (i = 0; i < cap; i++) {
        char ch = src[i];
        if (in_str) {
            if (esc)
                esc = 0;
            else if (ch == '\\')
                esc = 1;
            else if (ch == '"')
                in_str = 0;
            continue;
        }
        if (ch == '"') {
            in_str = 1;
            continue;
        }
        if (ch == ',')
            last_comma = i;
    }

    /* 关键：cut 取 last_comma + 1，把那个逗号本身也保留下来，
       否则预览会停在半个元素上（例如多出一个悬空的双引号） */
    cut = (last_comma != (size_t)-1 && last_comma > cap / 2) ? (last_comma + 1) : cap;

    r = (char *)malloc(cut + 1);
    if (!r)
        return -1;
    memcpy(r, src, cut);
    r[cut] = '\0';
    *out = r;
    if (out_len)
        *out_len = cut;
    if (truncated)
        *truncated = 1;
    return 0;
}

int jl_is_json(const char *s, size_t len)
{
    size_t i = 0;

    if (!s)
        return 0;
    if (len >= 3 && (unsigned char)s[0] == 0xEF &&
        (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        i = 3;
    while (i < len && jl_ws((unsigned char)s[i]))
        i++;
    if (i >= len)
        return 0;
    return s[i] == '{' || s[i] == '[';
}

/* ---------------------- 反转义 ---------------------- */

static int hex4(const char *s, unsigned *out)
{
    unsigned v = 0;
    int i;

    for (i = 0; i < 4; i++) {
        char ch = s[i];
        v <<= 4;
        if (ch >= '0' && ch <= '9')      v |= (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v |= (unsigned)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') v |= (unsigned)(ch - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static size_t utf8_put(char *dst, unsigned cp)
{
    if (cp < 0x80) {
        dst[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        dst[0] = (char)(0xE0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    dst[0] = (char)(0xF0 | (cp >> 18));
    dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    dst[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int jl_unescape(const char *s, size_t n, char **out, size_t *out_len)
{
    char *r;
    size_t i = 0, o = 0;

    if (!out)
        return -1;
    *out = NULL;
    if (out_len)
        *out_len = 0;
    if (!s && n)
        return -1;

    r = (char *)malloc(n + 1);
    if (!r)
        return -1;

    while (i < n) {
        char ch = s[i];
        if (ch != '\\' || i + 1 >= n) {
            r[o++] = ch;
            i++;
            continue;
        }
        ch = s[i + 1];
        switch (ch) {
        case '"':  r[o++] = '"';  i += 2; break;
        case '\\': r[o++] = '\\'; i += 2; break;
        case '/':  r[o++] = '/';  i += 2; break;
        case 'b':  r[o++] = '\b'; i += 2; break;
        case 'f':  r[o++] = '\f'; i += 2; break;
        case 'n':  r[o++] = '\n'; i += 2; break;
        case 'r':  r[o++] = '\r'; i += 2; break;
        case 't':  r[o++] = '\t'; i += 2; break;
        case 'u': {
            unsigned cp = 0;
            if (i + 6 <= n && hex4(s + i + 2, &cp) == 0) {
                i += 6;
                /* 代理对：高位 + 低位合成一个码点 */
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= n &&
                    s[i] == '\\' && s[i + 1] == 'u') {
                    unsigned lo = 0;
                    if (hex4(s + i + 2, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                o += utf8_put(r + o, cp);
            } else {
                r[o++] = ch;               /* 非法 \u：原样保留 */
                i += 2;
            }
            break;
        }
        default:
            r[o++] = ch;
            i += 2;
            break;
        }
    }
    r[o] = '\0';
    *out = r;
    if (out_len)
        *out_len = o;
    return 0;
}

/* ==================================================================
 * 内置自检（在目标机上跑，确认解析器没被工具链编坏）
 * ================================================================== */

int jl_selftest(char *report, size_t cch)
{
    size_t used = 0;
    int pass = 0, fail = 0;
    char *red = NULL;
    size_t redlen = 0;
    size_t imglen = 0;
    const char *payload = NULL;

    /* 构造一段"像 PNG"的 Base64：PNG 魔数 + 填充，长度必须 >= 256 */
    char pngb64[512];
    unsigned char pngraw[200];
    char json[2048];
    jl_image_list imgs;
    jl_summary sum;
    const char *keys[2];
    long code = -1;
    char msg[128];

#define JL_CHK(cond, name)                                                    \
    do {                                                                      \
        if (cond) { pass++; }                                                 \
        else {                                                                \
            fail++;                                                           \
            jl_rep(report, cch, &used, "  [失败] %s\n", name);                \
        }                                                                     \
    } while (0)

    if (!report || cch == 0)
        return 1;
    report[0] = '\0';

    memset(pngraw, 0, sizeof(pngraw));
    pngraw[0] = 0x89; pngraw[1] = 'P'; pngraw[2] = 'N'; pngraw[3] = 'G';
    pngraw[4] = 0x0D; pngraw[5] = 0x0A; pngraw[6] = 0x1A; pngraw[7] = 0x0A;
    b64_encode(pngraw, sizeof(pngraw), pngb64);

    jl_rep(report, cch, &used, "---- JSON 扫描器 ----\n");

    /* 基本字段提取 */
    keys[0] = "errorCode";
    keys[1] = "errorMsg";
    {
        char body[512];
        snprintf(body, sizeof(body),
                 "{\"logId\":\"x\",\"errorCode\":0,\"errorMsg\":\"Success\","
                 "\"result\":{\"n\":1}}");
        JL_CHK(jl_scan(body, strlen(body), keys, 2, &sum) == 0, "jl_scan 返回 0");
        JL_CHK(jl_field_int(&sum, "errorCode", &code) == 1 && code == 0,
               "取整数 errorCode = 0");
        JL_CHK(jl_field_str(&sum, "errorMsg", msg, sizeof(msg)) == 1 &&
               strcmp(msg, "Success") == 0, "取字符串 errorMsg");
        jl_summary_free(&sum);
    }

    /* 数字被写成字符串、errorMsg 含转义与中文 */
    {
        char body[512];
        snprintf(body, sizeof(body),
                 "{\"errorCode\":\"10001\",\"errorMsg\":\"\\u53c2\\u6570\\u9519\\u8bef \\\"file\\\"\"}");
        JL_CHK(jl_scan(body, strlen(body), keys, 2, &sum) == 0, "scan(字符串型 errorCode)");
        JL_CHK(jl_field_int(&sum, "errorCode", &code) == 1 && code == 10001,
               "字符串 \"10001\" 能转成 10001");
        JL_CHK(jl_field_str(&sum, "errorMsg", msg, sizeof(msg)) == 1 &&
               strcmp(msg, "参数错误 \"file\"") == 0, "\\u 转义与引号转义");
        jl_summary_free(&sum);
    }

    /* 嵌套里的同名 key 不应覆盖外层（只取首次） */
    {
        char body[512];
        snprintf(body, sizeof(body),
                 "{\"errorCode\":0,\"result\":{\"errorCode\":99}}");
        JL_CHK(jl_scan(body, strlen(body), keys, 1, &sum) == 0, "scan(嵌套同名 key)");
        JL_CHK(jl_field_int(&sum, "errorCode", &code) == 1 && code == 0,
               "嵌套同名 key 取外层首次值");
        jl_summary_free(&sum);
    }

    /* 图片识别：magic 命中 + 路径正确 */
    snprintf(json, sizeof(json),
             "{\"result\":{\"layoutParsingResults\":[{\"outputImages\":"
             "{\"layout_det_res\":\"%s\"}}]}}", pngb64);
    JL_CHK(jl_scan_images(json, strlen(json), &imgs) == 0, "scan_images 返回 0");
    JL_CHK(imgs.count == 1, "识别出 1 张 PNG");
    if (imgs.count == 1) {
        JL_CHK(strcmp(imgs.items[0].ext, ".png") == 0, "扩展名 .png（按魔数）");
        JL_CHK(strstr(imgs.items[0].path, "outputImages") != NULL &&
               strstr(imgs.items[0].path, "[0]") != NULL, "路径含数组下标与键名");
        payload = jl_image_payload_ptr(json, &imgs.items[0], &imglen);
        JL_CHK(payload && imglen == strlen(pngb64), "payload 指针与长度正确");
    }
    jl_image_list_free(&imgs);

    /* data URL 前缀 */
    snprintf(json, sizeof(json),
             "{\"a\":\"data:image/png;base64,%s\"}", pngb64);
    JL_CHK(jl_scan_images(json, strlen(json), &imgs) == 0, "scan_images(data URL)");
    JL_CHK(imgs.count == 1 && imgs.items[0].prefix_len > 0, "带 data 前缀仍能识别");
    jl_image_list_free(&imgs);

    /* 长 Markdown 文本不得被误判为图片 */
    {
        char big[1200];
        size_t i;
        for (i = 0; i < sizeof(big) - 1; i++)
            big[i] = (char)('a' + (char)(i % 26));
        big[sizeof(big) - 1] = '\0';
        snprintf(json, sizeof(json), "{\"markdown\":{\"text\":\"%s\"}}", big);
        JL_CHK(jl_scan_images(json, strlen(json), &imgs) == 0, "scan_images(长文本)");
        JL_CHK(imgs.count == 0, "长文本不被误判为图片");
        jl_image_list_free(&imgs);
    }

    /* 一长串 A（合法 Base64 但解出来不是图片）不得被误判 */
    {
        char as[600];
        memset(as, 'A', sizeof(as) - 1);
        as[sizeof(as) - 1] = '\0';
        snprintf(json, sizeof(json), "{\"k\":\"%s\"}", as);
        JL_CHK(jl_scan_images(json, strlen(json), &imgs) == 0, "scan_images(全 A)");
        JL_CHK(imgs.count == 0, "全 A 长串不被误判为图片");
        jl_image_list_free(&imgs);
    }

    /* 截断的 Base64 不得被误判（长度非 4 倍数） */
    {
        char cut[600];
        snprintf(cut, sizeof(cut), "%s", pngb64);
        cut[strlen(cut) - 1] = '\0';
        snprintf(json, sizeof(json), "{\"k\":\"%s\"}", cut);
        JL_CHK(jl_scan_images(json, strlen(json), &imgs) == 0, "scan_images(截断)");
        JL_CHK(imgs.count == 0, "截断的 Base64 不被误判");
        jl_image_list_free(&imgs);
    }

    /* 脱敏 */
    snprintf(json, sizeof(json),
             "{\"errorCode\":0,\"inputImage\":\"%s\",\"outputImages\":"
             "{\"a\":\"%s\"},\"markdown\":{\"text\":\"正文\"}}", pngb64, pngb64);
    JL_CHK(jl_scan_images(json, strlen(json), &imgs) == 0, "scan_images(脱敏用)");
    JL_CHK(imgs.count == 2, "脱敏用例识别出 2 张图");
    JL_CHK(jl_redact(json, strlen(json), &imgs, &red, &redlen) == 0, "jl_redact 返回 0");
    if (red) {
        JL_CHK(strstr(red, pngb64) == NULL, "脱敏后不再含原始 Base64");
        JL_CHK(strstr(red, "\"...\"") != NULL, "脱敏后被替换为 \"...\"");
        JL_CHK(strstr(red, "正文") != NULL, "普通文本保留");
        JL_CHK(strstr(red, "\"errorCode\":0") != NULL, "非图片字段保留");
        JL_CHK(redlen < strlen(json), "脱敏副本比原文短");
        free(red);
        red = NULL;
    }
    jl_image_list_free(&imgs);

    /* 预览截断 */
    {
        char *tp = NULL;
        size_t tlen = 0;
        int trunc = 0;
        JL_CHK(jl_truncate_preview(json, strlen(json), 60, &tp, &tlen, &trunc) == 0,
               "jl_truncate_preview 返回 0");
        JL_CHK(trunc == 1 && tlen <= 60, "预览确实被截断且不超长");
        free(tp);
    }

    /* is_json */
    JL_CHK(jl_is_json("{\"a\":1}", 7) == 1, "is_json(对象)");
    JL_CHK(jl_is_json("  [1]", 5) == 1, "is_json(数组，前导空白)");
    JL_CHK(jl_is_json("<html>", 6) == 0, "is_json(HTML) = 假");

    /* 非 JSON 输入不得崩、不得死循环 */
    {
        const char *junk = "this is not json at all {{{ [[[ \"unterminated";
        JL_CHK(jl_scan_images(junk, strlen(junk), &imgs) == 0, "非 JSON 输入不崩溃");
        JL_CHK(imgs.count == 0, "非 JSON 输入无图片");
        jl_image_list_free(&imgs);
    }

    /* 空输入 */
    JL_CHK(jl_find_int("", 0, "errorCode", &code) == 0, "空输入取值为假");
    JL_CHK(jl_find_int("{}", 2, "errorCode", &code) == 0, "空对象取值为假");

#undef JL_CHK

    jl_rep(report, cch, &used,
           "  通过 %d 项，失败 %d 项\n", pass, fail);
    return fail;
}
