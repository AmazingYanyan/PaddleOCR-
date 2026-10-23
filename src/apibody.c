/*
 * apibody.c - 参数表与请求体拼接
 *
 * 纯 C，不依赖任何平台 API，可在宿主机直接单测。
 * 生成顺序固定为：file -> fileType -> 参数表顺序，保证同样的开关状态永远得到
 * 逐字节相同的请求体（便于比对两次请求是否真的只差一个参数）。
 */
#include "apibody.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==================================================================
 * 参数表
 *
 * 说明：
 *   · AP_BOOL 的开关状态不写进表里，由 api_param_state.on 决定；
 *   · def_value 对 AP_NUM/AP_ENUM/AP_RAW 同时充当"默认填充值"，界面初始就填它；
 *   · tip 是中文说明，界面用作提示文字。
 * ================================================================== */
static const api_param_def PARAMS[] = {
    /* ---------------- 功能开关（值为 true/false） ---------------- */
    { "useLayoutDetection", AP_BOOL, AP_GROUP_SWITCH, NULL, NULL,
      "启用版面检测、区块定位与阅读顺序（正式调用应固定为 true）" },
    { "useDocOrientationClassify", AP_BOOL, AP_GROUP_SWITCH, NULL, NULL,
      "页面方向分类与纠正，扫描件建议测" },
    { "useDocUnwarping", AP_BOOL, AP_GROUP_SWITCH, NULL, NULL,
      "弯曲/透视矫正，拍照件建议测" },
    { "useChartRecognition", AP_BOOL, AP_GROUP_SWITCH, NULL, NULL,
      "统计图解析（柱状/折线/饼图）" },
    { "useSealRecognition", AP_BOOL, AP_GROUP_SWITCH, NULL, NULL,
      "印章识别" },
    { "useOcrForImageBlock", AP_BOOL, AP_GROUP_SWITCH, NULL, NULL,
      "对被判为图片的区块继续 OCR" },
    { "layoutNms", AP_BOOL, AP_GROUP_SWITCH, NULL, NULL,
      "用 NMS 去除重复/高度重叠的版面框" },
    { "mergeLayoutBlocks", AP_BOOL, AP_GROUP_SWITCH, NULL, NULL,
      "合并跨栏、错位的版面块" },
    { "formatBlockContent", AP_BOOL, AP_GROUP_SWITCH, NULL, NULL,
      "把区块内容格式化为 Markdown" },
    { "prettifyMarkdown", AP_BOOL, AP_GROUP_SWITCH, NULL, NULL,
      "美化 Markdown 排版" },
    { "showFormulaNumber", AP_BOOL, AP_GROUP_SWITCH, NULL, NULL,
      "Markdown 中保留公式编号" },
    { "restructurePages", AP_BOOL, AP_GROUP_SWITCH, NULL, NULL,
      "多页结果二次重组（第一阶段建议关闭）" },
    { "mergeTables", AP_BOOL, AP_GROUP_SWITCH, NULL, NULL,
      "合并跨页表格（需 restructurePages=true）" },
    { "relevelTitles", AP_BOOL, AP_GROUP_SWITCH, NULL, NULL,
      "重新整理多级标题（需 restructurePages=true）" },
    { "visualize", AP_BOOL, AP_GROUP_SWITCH, NULL, NULL,
      "返回可视化框图（会显著增大响应体）" },
    { "returnMarkdownImages", AP_BOOL, AP_GROUP_SWITCH, NULL, NULL,
      "返回 Markdown 引用的图片" },
    { "returnLayoutPolygonPoints", AP_BOOL, AP_GROUP_SWITCH, NULL, NULL,
      "返回 block_polygon_points 多边形坐标" },

    /* ---------------- 数值 / 枚举 ---------------- */
    { "layoutThreshold", AP_NUM, AP_GROUP_VALUE, "0.3", NULL,
      "版面框置信度阈值，0~1；越低漏检越少但误检越多" },
    { "layoutUnclipRatio", AP_NUM, AP_GROUP_VALUE, "1.5", NULL,
      "检测框扩张比例；可填单个数，也可填 [宽,高] 形式" },
    { "layoutMergeBboxesMode", AP_ENUM, AP_GROUP_VALUE, "large", "large|small|union",
      "重叠框取舍：large 留大框 / small 留小框 / union 全留" },
    { "layoutShapeMode", AP_ENUM, AP_GROUP_VALUE, "auto", "rect|quad|poly|auto",
      "坐标形态：矩形 / 四边形 / 多边形 / 自动" },
    { "maxNewTokens", AP_NUM, AP_GROUP_VALUE, "8192", NULL,
      "每个 VLM 任务最大生成 Token 数，重点测截断" },
    { "temperature", AP_NUM, AP_GROUP_VALUE, "0", NULL,
      "采样温度；OCR 场景保持低值或默认" },
    { "topP", AP_NUM, AP_GROUP_VALUE, "1.0", NULL,
      "核采样阈值，建议保持默认" },
    { "repetitionPenalty", AP_NUM, AP_GROUP_VALUE, "1.0", NULL,
      "重复惩罚，针对重复生成问题时测试" },
    { "minPixels", AP_NUM, AP_GROUP_VALUE, "", NULL,
      "输入 VLM 前允许的最小像素量（留空则沿用服务端配置）" },
    { "maxPixels", AP_NUM, AP_GROUP_VALUE, "", NULL,
      "输入 VLM 前允许的最大像素量（留空则沿用服务端配置）" },

    /* ---------------- 手写 JSON 片段 ---------------- */
    { "markdownIgnoreLabels", AP_RAW, AP_GROUP_RAW,
      "[\"number\",\"footnote\",\"header\",\"header_image\",\"footer\",\"footer_image\",\"aside_text\"]",
      NULL, "生成 Markdown 时忽略的版面类型（数组）" },
    { "vlmExtraArgs", AP_RAW, AP_GROUP_RAW, "{}", NULL,
      "按区块类型分别设置像素限制（对象）" },
    { "outputFormats", AP_RAW, AP_GROUP_RAW, "[\"docx\"]", NULL,
      "额外输出格式（数组，目前支持 docx）" }
};

#define PARAM_COUNT (sizeof(PARAMS) / sizeof(PARAMS[0]))

size_t api_param_count(void)
{
    return PARAM_COUNT;
}

const api_param_def *api_param_at(size_t i)
{
    if (i >= PARAM_COUNT)
        return NULL;
    return &PARAMS[i];
}

void api_params_default(api_param_state *params)
{
    size_t i;

    if (!params)
        return;
    memset(params, 0, sizeof(api_param_state) * API_MAX_PARAMS);
    for (i = 0; i < PARAM_COUNT; i++) {
        const api_param_def *d = &PARAMS[i];
        params[i].on = 0;
        params[i].value[0] = '\0';
        if (d->type != AP_BOOL && d->def_value)
            snprintf(params[i].value, API_VALUE_MAX, "%s", d->def_value);
    }
    /* 文档明确要求：正式调用时 useLayoutDetection 固定为 true，所以默认勾上 */
    for (i = 0; i < PARAM_COUNT; i++) {
        if (strcmp(PARAMS[i].name, "useLayoutDetection") == 0)
            params[i].on = 1;
    }
}

/* ==================================================================
 * 动态字符串缓冲
 * ================================================================== */
typedef struct {
    char  *p;
    size_t len;
    size_t cap;
    int    oom;
} sb_t;

static void sb_init(sb_t *b)
{
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = 0;
}

static int sb_reserve(sb_t *b, size_t extra)
{
    size_t need, ncap;
    char *np;

    if (b->oom)
        return 0;
    if (extra > API_MAX_BODY || b->len > API_MAX_BODY - extra) {
        b->oom = 1;
        return 0;
    }
    need = b->len + extra + 1;
    if (need <= b->cap)
        return 1;

    ncap = b->cap ? b->cap : 256;
    while (ncap < need) {
        if (ncap > API_MAX_BODY / 2) {
            ncap = API_MAX_BODY;
            break;
        }
        ncap *= 2;
    }
    if (ncap < need) {
        b->oom = 1;
        return 0;
    }
    np = (char *)realloc(b->p, ncap);
    if (!np) {
        b->oom = 1;
        return 0;
    }
    b->p = np;
    b->cap = ncap;
    return 1;
}

static void sb_put(sb_t *b, const char *s, size_t n)
{
    if (n == 0)
        return;
    if (!sb_reserve(b, n))
        return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_puts(sb_t *b, const char *s)
{
    if (s)
        sb_put(b, s, strlen(s));
}

static void sb_putc(sb_t *b, char c)
{
    sb_put(b, &c, 1);
}

/* 把字符串内容按 JSON 规则转义后写入（不含两侧引号） */
static void sb_put_escaped(sb_t *b, const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            char e[2];
            e[0] = '\\';
            e[1] = (char)c;
            sb_put(b, e, 2);
        } else if (c < 0x20) {
            char e[8];
            snprintf(e, sizeof(e), "\\u%04x", (unsigned)c);
            sb_puts(b, e);
        } else {
            sb_putc(b, (char)c);
        }
    }
}

/* ==================================================================
 * 值校验
 * ================================================================== */

static void set_err(char *err, size_t errc, const char *fmt, ...)
{
    va_list ap;

    if (!err || errc == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(err, errc, fmt, ap);
    va_end(ap);
    err[errc - 1] = '\0';
}

static int is_blank(char c)
{
    return c == ' ' || c == '\t';
}

/*
 * 严格按 JSON 的数字文法校验：-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
 *
 * 这里刻意不用 strtod + "是否被完整消费"来判断：strtod 会接受 "1."、".5"、"007"，
 * 而这些写法拼进请求体就是非法 JSON，服务端会直接报参数错误。
 * 宁可在这里拒绝，也不要把一个坏请求发出去。
 */
static const char *json_number_end(const char *p)
{
    if (!p || !*p)
        return NULL;
    if (*p == '-')
        p++;
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (*p >= '0' && *p <= '9')
            p++;
    } else {
        return NULL;
    }
    if (*p == '.') {
        p++;
        if (!(*p >= '0' && *p <= '9'))
            return NULL;
        while (*p >= '0' && *p <= '9')
            p++;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-')
            p++;
        if (!(*p >= '0' && *p <= '9'))
            return NULL;
        while (*p >= '0' && *p <= '9')
            p++;
    }
    return p;
}

static int is_number(const char *s)
{
    const char *p = s;
    const char *end;

    if (!s)
        return 0;
    while (is_blank(*p))
        p++;
    end = json_number_end(p);
    if (!end)
        return 0;
    while (is_blank(*end))
        end++;
    return *end == '\0';
}

/* [a,b] 或 [a,b,c] 形式的数字数组（layoutUnclipRatio 用） */
static int is_number_list(const char *s)
{
    const char *p = s;
    int count = 0;

    if (!s)
        return 0;
    while (is_blank(*p))
        p++;
    if (*p != '[')
        return 0;
    p++;
    for (;;) {
        const char *end;

        while (is_blank(*p))
            p++;
        end = json_number_end(p);
        if (!end)
            return 0;
        p = end;
        count++;
        while (is_blank(*p))
            p++;
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == ']') {
            p++;
            break;
        }
        return 0;
    }
    while (is_blank(*p))
        p++;
    if (*p)
        return 0;
    return count >= 1 && count <= 4;
}

static int is_valid_number(const char *s)
{
    return is_number(s) || is_number_list(s);
}

static int enum_ok(const char *opts, const char *val)
{
    size_t vlen;
    const char *p;

    if (!opts || !val || !val[0])
        return 0;
    vlen = strlen(val);
    p = opts;
    while (*p) {
        const char *bar = strchr(p, '|');
        size_t seg = bar ? (size_t)(bar - p) : strlen(p);
        if (seg == vlen && memcmp(p, val, vlen) == 0)
            return 1;
        if (!bar)
            break;
        p = bar + 1;
    }
    return 0;
}

/* 手写 JSON 片段：必须以 [ 或 { 开头，且括号配平、字符串闭合 */
static int is_balanced_json(const char *s)
{
    const char *p = s;
    int depth = 0, in_str = 0, esc = 0;

    if (!s)
        return 0;
    while (is_blank(*p))
        p++;
    if (*p != '[' && *p != '{')
        return 0;

    for (; *p; p++) {
        char ch = *p;
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
        if (ch == '[' || ch == '{')
            depth++;
        else if (ch == ']' || ch == '}') {
            depth--;
            if (depth < 0)
                return 0;
        }
    }
    return depth == 0 && !in_str;
}

/* ==================================================================
 * 文件类型映射
 * ================================================================== */

int api_filetype_for_ext(const char *ext, int *known)
{
    static const char *imgs[] = {
        ".png", ".jpg", ".gif", ".bmp", ".webp", ".tif", ".ico"
    };
    size_t i;

    if (known)
        *known = 0;
    if (!ext || !ext[0]) {
        if (known)
            *known = 0;
        return 1;                  /* 未知：按图片处理，由用户在界面上改 */
    }
    if (strcmp(ext, ".pdf") == 0) {
        if (known)
            *known = 1;
        return 0;
    }
    for (i = 0; i < sizeof(imgs) / sizeof(imgs[0]); i++) {
        if (strcmp(ext, imgs[i]) == 0) {
            if (known)
                *known = 1;
            return 1;
        }
    }
    return 1;                      /* 未知类型：按 1 处理，但 known 保持 0 */
}

/* ==================================================================
 * 请求体拼接
 * ================================================================== */

int api_build_body(const api_request_spec *spec, char **out, size_t *out_len,
                   char *err, size_t errc)
{
    sb_t b;
    size_t i;

    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    if (err && errc)
        err[0] = '\0';

    if (!spec) {
        set_err(err, errc, "内部错误：请求配置为空");
        return -1;
    }

    /* 约定：GET 不带 body */
    if (spec->method_get)
        return 0;

    if (!spec->file_b64 || spec->file_b64_len == 0) {
        set_err(err, errc, "还没有选择文件（file 为空）");
        return -1;
    }
    if (spec->file_type != 0 && spec->file_type != 1) {
        set_err(err, errc, "fileType 只能是 0（PDF）或 1（图片），当前为 %d",
                spec->file_type);
        return -1;
    }

    sb_init(&b);
    sb_putc(&b, '{');

    /* file */
    sb_puts(&b, "\"file\":\"");
    sb_put_escaped(&b, spec->file_b64, spec->file_b64_len);
    sb_putc(&b, '"');

    /* fileType */
    sb_puts(&b, ",\"fileType\":");
    sb_putc(&b, spec->file_type ? '1' : '0');

    for (i = 0; i < PARAM_COUNT; i++) {
        const api_param_def *d = &PARAMS[i];
        const api_param_state *st = &spec->params[i];
        const char *val;

        if (!st->on)
            continue;

        if (d->type == AP_BOOL) {
            sb_putc(&b, ',');
            sb_putc(&b, '"');
            sb_puts(&b, d->name);
            sb_puts(&b, "\":true");
            continue;
        }

        val = st->value;
        if (!val[0] && d->def_value)
            val = d->def_value;

        switch (d->type) {
        case AP_NUM:
            if (!val[0]) {
                set_err(err, errc, "参数 %s 需要一个数值，当前为空", d->name);
                free(b.p);
                return -1;
            }
            if (!is_valid_number(val)) {
                set_err(err, errc,
                        "参数 %s 的值『%s』不是合法数字（小数请用点号，"
                        "例如 0.3；数组请写成 [1.1,1.2]）", d->name, val);
                free(b.p);
                return -1;
            }
            break;

        case AP_ENUM:
            if (!enum_ok(d->opts, val)) {
                set_err(err, errc, "参数 %s 的值『%s』不在候选范围内（可选：%s）",
                        d->name, val, d->opts ? d->opts : "");
                free(b.p);
                return -1;
            }
            break;

        case AP_RAW:
            if (!is_balanced_json(val)) {
                set_err(err, errc,
                        "参数 %s 的手写片段不合法：必须以 [ 或 { 开头，"
                        "括号与引号要成对闭合", d->name);
                free(b.p);
                return -1;
            }
            break;

        case AP_BOOL:
        default:
            break;
        }

        sb_putc(&b, ',');
        sb_putc(&b, '"');
        sb_puts(&b, d->name);
        sb_puts(&b, "\":");
        /*
         * 枚举是字符串，必须带引号；数值与手写 JSON 片段是裸 token。
         * （枚举忘了加引号会拼出 "layoutShapeMode":quad 这种非法 JSON，
         *   服务端只会回一个含糊的解析错误，很难查。）
         */
        if (d->type == AP_ENUM) {
            sb_putc(&b, '"');
            sb_put_escaped(&b, val, strlen(val));
            sb_putc(&b, '"');
        } else {
            sb_puts(&b, val);
        }
    }

    sb_putc(&b, '}');

    if (b.oom || !b.p) {
        free(b.p);
        set_err(err, errc,
                "请求体过大（上限 %lu 字节）或内存不足，已中止",
                (unsigned long)API_MAX_BODY);
        return -1;
    }
    if (b.len > API_MAX_BODY) {
        free(b.p);
        set_err(err, errc, "请求体超过上限（%lu 字节）",
                (unsigned long)API_MAX_BODY);
        return -1;
    }

    if (out)
        *out = b.p;
    else
        free(b.p);
    if (out_len)
        *out_len = b.len;
    return 0;
}

/* ==================================================================
 * 内置自检
 * ================================================================== */

int api_selftest(char *report, size_t cch)
{
    size_t used = 0;
    int pass = 0, fail = 0;
    api_request_spec spec;
    char err[512];
    char *body = NULL;
    size_t blen = 0;
    size_t i;

#define API_CHK(cond, name)                                                   \
    do {                                                                      \
        if (cond) { pass++; }                                                 \
        else {                                                                \
            fail++;                                                           \
            if (report && used < cch - 1)                                     \
                used += (size_t)snprintf(report + used, cch - used,           \
                                         "  [失败] %s\n", name);              \
        }                                                                     \
    } while (0)

    if (!report || cch == 0)
        return 1;
    report[0] = '\0';

    if (cch > 0) {
        int n = snprintf(report, cch, "---- 请求体拼接 ----\n");
        used = (n > 0 && (size_t)n < cch) ? (size_t)n : 0;
    }

    /* 参数表完整性 */
    API_CHK(api_param_count() >= 30, "参数表至少 30 项");
    {
        int bools = 0, vals = 0, raws = 0;
        for (i = 0; i < api_param_count(); i++) {
            const api_param_def *d = api_param_at(i);
            if (!d || !d->name)
                continue;
            if (d->type == AP_BOOL) bools++;
            else if (d->type == AP_RAW) raws++;
            else vals++;
        }
        API_CHK(bools == 17, "布尔参数 17 项");
        API_CHK(vals == 10, "数值/枚举参数 10 项");
        API_CHK(raws == 3, "手写片段参数 3 项");
    }

    memset(&spec, 0, sizeof(spec));
    api_params_default(spec.params);

    /* 默认状态：只有 useLayoutDetection 勾选 */
    {
        int on_count = 0;
        for (i = 0; i < api_param_count(); i++)
            if (spec.params[i].on) on_count++;
        API_CHK(on_count == 1, "默认只勾选 1 个开关");
        API_CHK(spec.params[0].on == 1 &&
                strcmp(api_param_at(0)->name, "useLayoutDetection") == 0,
                "默认勾选的是 useLayoutDetection");
    }

    /* 默认 body 精确比对 */
    spec.file_b64 = "QUJD";
    spec.file_b64_len = 4;
    spec.file_type = 1;
    API_CHK(api_build_body(&spec, &body, &blen, err, sizeof(err)) == 0,
            "默认配置能生成 body");
    API_CHK(body && strcmp(body,
            "{\"file\":\"QUJD\",\"fileType\":1,\"useLayoutDetection\":true}") == 0,
            "默认 body 逐字节等于预期");
    API_CHK(blen == (body ? strlen(body) : 0), "返回长度与字符串长度一致");
    free(body);
    body = NULL;

    /* PDF 映射 */
    API_CHK(api_filetype_for_ext(".pdf", NULL) == 0, ".pdf -> fileType 0");
    API_CHK(api_filetype_for_ext(".png", NULL) == 1, ".png -> fileType 1");
    API_CHK(api_filetype_for_ext(".tif", NULL) == 1, ".tif -> fileType 1");
    {
        int known = 1;
        api_filetype_for_ext(".bin", &known);
        API_CHK(known == 0, "未知扩展名标记为未识别");
    }

    /* 关掉唯一开关 -> 只剩 file/fileType */
    spec.params[0].on = 0;
    API_CHK(api_build_body(&spec, &body, &blen, err, sizeof(err)) == 0, "关闭开关后能生成");
    API_CHK(body && strcmp(body, "{\"file\":\"QUJD\",\"fileType\":1}") == 0,
            "全部关闭时 body 只有两个字段");
    free(body);
    body = NULL;

    /* GET 不带 body */
    spec.method_get = 1;
    API_CHK(api_build_body(&spec, &body, &blen, err, sizeof(err)) == 0,
            "GET 返回成功");
    API_CHK(body == NULL && blen == 0, "GET 不生成 body");
    spec.method_get = 0;

    /* 打开数值 + 枚举 + 片段 */
    for (i = 0; i < api_param_count(); i++) {
        const char *nm = api_param_at(i)->name;
        if (strcmp(nm, "layoutThreshold") == 0) spec.params[i].on = 1;
        if (strcmp(nm, "layoutShapeMode") == 0) spec.params[i].on = 1;
        if (strcmp(nm, "markdownIgnoreLabels") == 0) spec.params[i].on = 1;
    }
    API_CHK(api_build_body(&spec, &body, &blen, err, sizeof(err)) == 0,
            "数值/枚举/片段混合能生成");
    API_CHK(body && strstr(body, "\"layoutThreshold\":0.3") != NULL,
            "数值参数按数字写入（无引号）");
    API_CHK(body && strstr(body, "\"layoutShapeMode\":\"auto\"") != NULL,
            "枚举参数按字符串写入");
    API_CHK(body && strstr(body, "\"markdownIgnoreLabels\":[\"number\"") != NULL,
            "手写片段原样写入");
    API_CHK(body && body[strlen(body) - 1] == '}', "body 以 } 结尾");
    free(body);
    body = NULL;

    /* 非法值必须被拒绝 */
    {
        int idx = -1;
        for (i = 0; i < api_param_count(); i++)
            if (strcmp(api_param_at(i)->name, "layoutThreshold") == 0) idx = (int)i;
        API_CHK(idx >= 0, "找到 layoutThreshold 项");
        if (idx >= 0) {
            spec.params[idx].on = 1;
            snprintf(spec.params[idx].value, API_VALUE_MAX, "1,5");
            API_CHK(api_build_body(&spec, &body, &blen, err, sizeof(err)) == -1,
                    "逗号小数的数值被拒绝");
            API_CHK(err[0] != '\0', "拒绝时给出错误原因");

            snprintf(spec.params[idx].value, API_VALUE_MAX, "abc");
            API_CHK(api_build_body(&spec, &body, &blen, err, sizeof(err)) == -1,
                    "非数字文本被拒绝");

            /* 留空：有默认值的参数回落到默认，而不是报错 */
            snprintf(spec.params[idx].value, API_VALUE_MAX, "");
            API_CHK(api_build_body(&spec, &body, &blen, err, sizeof(err)) == 0,
                    "留空时有默认值的参数回落到默认值");
            API_CHK(body && strstr(body, "\"layoutThreshold\":0.3") != NULL,
                    "回落值确实是 0.3");
            free(body);
            body = NULL;

            /* 没有默认值的参数留空必须报错，绝不能把空值发出去 */
            {
                int mi = -1;
                size_t k;
                for (k = 0; k < api_param_count(); k++) {
                    if (strcmp(api_param_at(k)->name, "minPixels") == 0)
                        mi = (int)k;
                }
                if (mi >= 0) {
                    spec.params[mi].on = 1;
                    spec.params[mi].value[0] = '\0';
                    API_CHK(api_build_body(&spec, &body, &blen, err, sizeof(err)) == -1,
                            "无默认值的参数留空被拒绝");
                    spec.params[mi].on = 0;
                }
            }

            snprintf(spec.params[idx].value, API_VALUE_MAX, "0.5");
            API_CHK(api_build_body(&spec, &body, &blen, err, sizeof(err)) == 0,
                    "合法数值通过");
            free(body);
            body = NULL;
        }
    }

    /* layoutUnclipRatio 的数组形式 */
    {
        int idx = -1;
        for (i = 0; i < api_param_count(); i++)
            if (strcmp(api_param_at(i)->name, "layoutUnclipRatio") == 0) idx = (int)i;
        if (idx >= 0) {
            spec.params[idx].on = 1;
            snprintf(spec.params[idx].value, API_VALUE_MAX, "[1.1,1.2]");
            API_CHK(api_build_body(&spec, &body, &blen, err, sizeof(err)) == 0,
                    "[1.1,1.2] 数组形式被接受");
            API_CHK(body && strstr(body, "\"layoutUnclipRatio\":[1.1,1.2]") != NULL,
                    "数组形式原样写入");
            free(body);
            body = NULL;
            spec.params[idx].on = 0;
        }
    }

    /* 非法枚举 */
    {
        int idx = -1;
        for (i = 0; i < api_param_count(); i++)
            if (strcmp(api_param_at(i)->name, "layoutShapeMode") == 0) idx = (int)i;
        if (idx >= 0) {
            snprintf(spec.params[idx].value, API_VALUE_MAX, "huge");
            API_CHK(api_build_body(&spec, &body, &blen, err, sizeof(err)) == -1,
                    "非法枚举值被拒绝");
            snprintf(spec.params[idx].value, API_VALUE_MAX, "quad");
            API_CHK(api_build_body(&spec, &body, &blen, err, sizeof(err)) == 0,
                    "合法枚举值通过");
            free(body);
            body = NULL;
        }
    }

    /* 非法 JSON 片段 */
    {
        int idx = -1;
        for (i = 0; i < api_param_count(); i++)
            if (strcmp(api_param_at(i)->name, "vlmExtraArgs") == 0) idx = (int)i;
        if (idx >= 0) {
            spec.params[idx].on = 1;
            snprintf(spec.params[idx].value, API_VALUE_MAX, "{}");
            API_CHK(api_build_body(&spec, &body, &blen, err, sizeof(err)) == 0,
                    "空对象片段被接受");
            free(body);
            body = NULL;

            snprintf(spec.params[idx].value, API_VALUE_MAX, "[1,2");
            API_CHK(api_build_body(&spec, &body, &blen, err, sizeof(err)) == -1,
                    "括号不配平的片段被拒绝");

            snprintf(spec.params[idx].value, API_VALUE_MAX, "hello");
            API_CHK(api_build_body(&spec, &body, &blen, err, sizeof(err)) == -1,
                    "不以 [ 或 { 开头的片段被拒绝");
            spec.params[idx].on = 0;
        }
    }

    /* fileType 越界 */
    spec.file_type = 2;
    API_CHK(api_build_body(&spec, &body, &blen, err, sizeof(err)) == -1,
            "fileType=2 被拒绝");
    spec.file_type = 1;

    /* 空文件 */
    spec.file_b64 = NULL;
    spec.file_b64_len = 0;
    API_CHK(api_build_body(&spec, &body, &blen, err, sizeof(err)) == -1,
            "没有文件时被拒绝");

    /* 大 Base64：长度与结尾正确 */
    {
        size_t n = 1024 * 1024;
        char *big = (char *)malloc(n + 1);
        if (big) {
            memset(big, 'A', n);
            big[n] = '\0';
            spec.file_b64 = big;
            spec.file_b64_len = n;
            /* 清掉所有开关，方便算长度 */
            for (i = 0; i < api_param_count(); i++)
                spec.params[i].on = 0;
            API_CHK(api_build_body(&spec, &body, &blen, err, sizeof(err)) == 0,
                    "1MB Base64 能生成 body");
            API_CHK(body && blen == n + 24, "1MB body 长度符合预期（n+24）");
            API_CHK(body && body[blen - 1] == '}', "1MB body 结尾正确");
            free(body);
            body = NULL;
            free(big);
        }
    }

    /* file 内容里的引号要被转义（防御性，正常 Base64 不会有） */
    {
        spec.file_b64 = "ab\"cd";
        spec.file_b64_len = 5;
        API_CHK(api_build_body(&spec, &body, &blen, err, sizeof(err)) == 0,
                "含引号的 file 能生成");
        API_CHK(body && strstr(body, "ab\\\"cd") != NULL, "引号被转义");
        free(body);
        body = NULL;
    }

#undef API_CHK

    if (report && used < cch - 1)
        snprintf(report + used, cch - used, "  通过 %d 项，失败 %d 项\n", pass, fail);
    return fail;
}
