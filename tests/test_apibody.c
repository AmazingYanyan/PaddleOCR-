/*
 * test_apibody.c - 参数表与请求体拼接单元测试（宿主机直接跑）
 *
 * 重点不是"能拼出来"，而是"拼错了能被拦住"：
 *   · 非 JSON 数字文法的值（1. / .5 / 007 / 1,5）必须拒绝
 *   · 非法枚举、不配平的手写 JSON 片段必须拒绝
 *   · 生成出来的 body 必须能通过我们自己的 JSON 扫描器（交叉验证）
 *
 * 编译运行：
 *   cc -std=c99 -O2 -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion \
 *      -fsanitize=undefined -o build/test_apibody \
 *      tests/test_apibody.c src/apibody.c src/jsonlite.c src/b64sha.c
 *   ./build/test_apibody
 */
#include "../src/apibody.h"
#include "../src/jsonlite.h"

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

static int idx_of(const char *name)
{
    size_t i;
    for (i = 0; i < api_param_count(); i++) {
        const api_param_def *d = api_param_at(i);
        if (d && strcmp(d->name, name) == 0)
            return (int)i;
    }
    return -1;
}

static void set_on(api_request_spec *s, const char *name, const char *val)
{
    int i = idx_of(name);
    if (i < 0) {
        printf("  [内部错误] 找不到参数 %s\n", name);
        return;
    }
    s->params[i].on = 1;
    if (val)
        snprintf(s->params[i].value, API_VALUE_MAX, "%s", val);
}

static void all_off(api_request_spec *s)
{
    size_t i;
    for (i = 0; i < api_param_count(); i++)
        s->params[i].on = 0;
}

static void base_spec(api_request_spec *s)
{
    memset(s, 0, sizeof(*s));
    api_params_default(s->params);
    s->file_b64 = "QUJD";
    s->file_b64_len = 4;
    s->file_type = 1;
}

static int build(api_request_spec *s, char **body, size_t *len, char *err, size_t errc)
{
    return api_build_body(s, body, len, err, errc);
}

/* ---------------------------------------------------------------- */

static void t_table(void)
{
    int i, b = 0, v = 0, r = 0;

    ok(api_param_count() >= 30, "参数表至少 30 项");
    for (i = 0; i < (int)api_param_count(); i++) {
        const api_param_def *d = api_param_at(i);
        ok(d != NULL && d->name != NULL && d->name[0] != 0, "每项都有字段名");
        if (!d)
            continue;
        if (d->type == AP_BOOL) b++;
        else if (d->type == AP_RAW) r++;
        else v++;
        if (d->type == AP_ENUM)
            ok(d->opts != NULL && strchr(d->opts, '|') != NULL, "枚举项有候选值");
        if (d->type == AP_RAW)
            ok(d->def_value != NULL, "片段项有预填值");
    }
    ok(b == 17, "布尔参数 17 项");
    ok(v == 10, "数值/枚举参数 10 项");
    ok(r == 3, "手写片段参数 3 项");
    ok(api_param_at(api_param_count()) == NULL, "越界取参数返回 NULL");
}

static void t_defaults(void)
{
    api_param_state ps[API_MAX_PARAMS];
    int i, on = 0;

    api_params_default(ps);
    for (i = 0; i < (int)api_param_count(); i++)
        if (ps[i].on) on++;
    ok(on == 1, "默认只勾选一个开关");
    ok(ps[idx_of("useLayoutDetection")].on == 1, "默认勾选 useLayoutDetection");
    ok(ps[idx_of("visualize")].on == 0, "默认不勾选 visualize");
    eqs(ps[idx_of("layoutThreshold")].value, "0.3", "layoutThreshold 预填 0.3");
    eqs(ps[idx_of("layoutShapeMode")].value, "auto", "layoutShapeMode 预填 auto");
    ok(strstr(ps[idx_of("markdownIgnoreLabels")].value, "footnote") != NULL,
       "markdownIgnoreLabels 预填了文档默认值");
}

static void t_exact_body(void)
{
    api_request_spec s;
    char err[512];
    char *body = NULL;
    size_t len = 0;

    base_spec(&s);
    ok(build(&s, &body, &len, err, sizeof(err)) == 0, "默认配置生成成功");
    eqs(body, "{\"file\":\"QUJD\",\"fileType\":1,\"useLayoutDetection\":true}",
        "默认 body 逐字节正确");
    ok(len == (body ? strlen(body) : 0), "返回长度与 strlen 一致");
    ok(jl_is_json(body, len) == 1, "生成的 body 是合法 JSON");
    free(body);
    body = NULL;

    all_off(&s);
    ok(build(&s, &body, &len, err, sizeof(err)) == 0, "全部关闭生成成功");
    eqs(body, "{\"file\":\"QUJD\",\"fileType\":1}", "全部关闭只剩 file/fileType");
    free(body);
    body = NULL;

    s.file_type = 0;
    ok(build(&s, &body, &len, err, sizeof(err)) == 0, "PDF 生成成功");
    eqs(body, "{\"file\":\"QUJD\",\"fileType\":0}", "fileType 0 正确输出");
    free(body);
    body = NULL;
}

static void t_get(void)
{
    api_request_spec s;
    char err[512];
    char *body = (char *)0x1;
    size_t len = 999;

    base_spec(&s);
    s.method_get = 1;
    ok(build(&s, &body, &len, err, sizeof(err)) == 0, "GET 返回成功");
    ok(body == NULL, "GET 不生成 body");
    ok(len == 0, "GET 长度为 0");
}

static void t_all_bools(void)
{
    api_request_spec s;
    char err[512];
    char *body = NULL;
    size_t len = 0, i;
    int present = 0, total = 0;

    base_spec(&s);
    all_off(&s);
    for (i = 0; i < api_param_count(); i++) {
        if (api_param_at(i)->type == AP_BOOL) {
            s.params[i].on = 1;
            total++;
        }
    }
    ok(build(&s, &body, &len, err, sizeof(err)) == 0, "全部布尔开关打开能生成");
    if (body) {
        for (i = 0; i < api_param_count(); i++) {
            const api_param_def *d = api_param_at(i);
            if (d->type != AP_BOOL)
                continue;
            if (strstr(body, d->name) != NULL)
                present++;
        }
        ok(present == total, "17 个布尔字段全部出现在 body 里");
        ok(strstr(body, ":false") == NULL, "关闭的开关不会被写成 false");
        ok(strstr(body, "\":true\"") == NULL, "true 不加引号（是布尔不是字符串）");
        ok(jl_is_json(body, len) == 1, "全布尔 body 仍是合法 JSON");
        free(body);
    }
}

static void t_numbers_ok(void)
{
    static const char *good[] = { "0", "0.3", "1.0", "8192", "-0.5", "1e3", "2E-2", "0.0" };
    size_t i;
    char err[512];

    for (i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        api_request_spec s;
        char *body = NULL;
        size_t len = 0;
        char what[128];

        base_spec(&s);
        all_off(&s);
        set_on(&s, "layoutThreshold", good[i]);
        snprintf(what, sizeof(what), "合法数值 %s 被接受", good[i]);
        ok(build(&s, &body, &len, err, sizeof(err)) == 0, what);
        if (body) {
            char want[160];
            snprintf(want, sizeof(want), "\"layoutThreshold\":%s", good[i]);
            ok(strstr(body, want) != NULL && jl_is_json(body, len) == 1,
               "数值原样写入且 JSON 合法");
            free(body);
        }
    }
}

static void t_numbers_bad(void)
{
    static const char *bad[] = {
        "1.", ".5", "007", "1,5", "abc", " ", "+1", "0x10", "1e", "--1", "NaN", "Infinity", "1 2"
    };
    size_t i;
    char err[512];

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        api_request_spec s;
        char *body = NULL;
        size_t len = 0;
        char what[160];

        base_spec(&s);
        all_off(&s);
        set_on(&s, "layoutThreshold", bad[i]);
        snprintf(what, sizeof(what), "非法数值『%s』被拒绝", bad[i]);
        ok(build(&s, &body, &len, err, sizeof(err)) == -1, what);
        ok(err[0] != '\0', "拒绝时给出了错误原因");
        if (body) {
            free(body);
            body = NULL;
        }
    }
}

static void t_empty_value(void)
{
    api_request_spec s;
    char err[512];
    char *body = NULL;
    size_t len = 0;

    /* 有默认值的参数留空：回落到默认值 */
    base_spec(&s);
    all_off(&s);
    set_on(&s, "layoutThreshold", "");
    ok(build(&s, &body, &len, err, sizeof(err)) == 0, "留空时回落到默认值");
    ok(body && strstr(body, "\"layoutThreshold\":0.3") != NULL, "回落值是 0.3");
    free(body);
    body = NULL;

    /* 没有默认值的参数留空：必须报错，不能发一个空值出去 */
    base_spec(&s);
    all_off(&s);
    set_on(&s, "minPixels", "");
    ok(build(&s, &body, &len, err, sizeof(err)) == -1, "无默认值的参数留空被拒绝");
    ok(strstr(err, "minPixels") != NULL, "错误信息点名 minPixels");
}

static void t_lists(void)
{
    api_request_spec s;
    char err[512];
    char *body = NULL;
    size_t len = 0;

    base_spec(&s);
    all_off(&s);
    set_on(&s, "layoutUnclipRatio", "[1.1,1.2]");
    ok(build(&s, &body, &len, err, sizeof(err)) == 0, "[1.1,1.2] 被接受");
    ok(body && strstr(body, "\"layoutUnclipRatio\":[1.1,1.2]") != NULL, "数组原样写入");
    ok(body && jl_is_json(body, len) == 1, "数组 body 合法");
    free(body);
    body = NULL;

    set_on(&s, "layoutUnclipRatio", "[ 1.1 , 1.2 ]");
    ok(build(&s, &body, &len, err, sizeof(err)) == 0, "带空格的数组被接受");
    free(body);
    body = NULL;

    set_on(&s, "layoutUnclipRatio", "1.5");
    ok(build(&s, &body, &len, err, sizeof(err)) == 0, "单个数也接受");
    free(body);
    body = NULL;

    set_on(&s, "layoutUnclipRatio", "[]");
    ok(build(&s, &body, &len, err, sizeof(err)) == -1, "空数组被拒绝");

    set_on(&s, "layoutUnclipRatio", "[1,]");
    ok(build(&s, &body, &len, err, sizeof(err)) == -1, "尾随逗号的数组被拒绝");

    set_on(&s, "layoutUnclipRatio", "[1;2]");
    ok(build(&s, &body, &len, err, sizeof(err)) == -1, "分号分隔的数组被拒绝");

    set_on(&s, "layoutUnclipRatio", "[1,2");
    ok(build(&s, &body, &len, err, sizeof(err)) == -1, "未闭合的数组被拒绝");
}

static void t_enums(void)
{
    api_request_spec s;
    char err[512];
    char *body = NULL;
    size_t len = 0;

    base_spec(&s);
    all_off(&s);

    set_on(&s, "layoutShapeMode", "quad");
    ok(build(&s, &body, &len, err, sizeof(err)) == 0, "合法枚举 quad 被接受");
    ok(body && strstr(body, "\"layoutShapeMode\":\"quad\"") != NULL, "枚举写成字符串");
    free(body);
    body = NULL;

    set_on(&s, "layoutShapeMode", "poly");
    ok(build(&s, &body, &len, err, sizeof(err)) == 0, "合法枚举 poly 被接受");
    free(body);
    body = NULL;

    set_on(&s, "layoutShapeMode", "huge");
    ok(build(&s, &body, &len, err, sizeof(err)) == -1, "非法枚举被拒绝");

    set_on(&s, "layoutShapeMode", "QUAD");
    ok(build(&s, &body, &len, err, sizeof(err)) == -1, "枚举大小写敏感（QUAD 拒绝）");

    /* 恢复成合法值，避免上一行的非法状态影响下一个用例 */
    s.params[idx_of("layoutShapeMode")].on = 0;
    set_on(&s, "layoutMergeBboxesMode", "union");
    ok(build(&s, &body, &len, err, sizeof(err)) == 0, "large|small|union 中的 union 被接受");
    free(body);
    body = NULL;
}

static void t_raw(void)
{
    api_request_spec s;
    char err[512];
    char *body = NULL;
    size_t len = 0;

    base_spec(&s);
    all_off(&s);

    set_on(&s, "vlmExtraArgs", "{}");
    ok(build(&s, &body, &len, err, sizeof(err)) == 0, "{} 被接受");
    free(body);
    body = NULL;

    set_on(&s, "vlmExtraArgs", "{\"text\":{\"max_pixels\":1000}}");
    ok(build(&s, &body, &len, err, sizeof(err)) == 0, "嵌套对象被接受");
    ok(body && jl_is_json(body, len) == 1, "嵌套对象 body 合法");
    /* 用扫描器确认能读到嵌套值 */
    {
        long v = 0;
        ok(jl_find_int(body, len, "max_pixels", &v) == 1 && v == 1000,
           "嵌套对象里的值可被解析");
    }
    free(body);
    body = NULL;

    set_on(&s, "vlmExtraArgs", "[\"a\",\"b\"]");
    ok(build(&s, &body, &len, err, sizeof(err)) == 0, "数组片段被接受");
    free(body);
    body = NULL;

    set_on(&s, "vlmExtraArgs", "[1,2");
    ok(build(&s, &body, &len, err, sizeof(err)) == -1, "不配平的片段被拒绝");

    set_on(&s, "vlmExtraArgs", "hello");
    ok(build(&s, &body, &len, err, sizeof(err)) == -1, "不以括号开头的片段被拒绝");

    set_on(&s, "vlmExtraArgs", "{\"a\":\"未闭合}");
    ok(build(&s, &body, &len, err, sizeof(err)) == -1, "字符串未闭合的片段被拒绝");

    set_on(&s, "vlmExtraArgs", "}]");
    ok(build(&s, &body, &len, err, sizeof(err)) == -1, "多余的闭合括号被拒绝");
}

static void t_escape_and_size(void)
{
    api_request_spec s;
    char err[512];
    char *body = NULL;
    size_t len = 0;

    base_spec(&s);
    all_off(&s);

    /* 正常 Base64 不含引号，但万一传进来要转义，不能拼出坏 JSON */
    s.file_b64 = "ab\"cd\\ef";
    s.file_b64_len = 8;
    ok(build(&s, &body, &len, err, sizeof(err)) == 0, "含特殊字符的 file 能生成");
    ok(body && strstr(body, "ab\\\"cd\\\\ef") != NULL, "引号与反斜杠被转义");
    ok(body && jl_is_json(body, len) == 1, "转义后仍是合法 JSON");
    free(body);
    body = NULL;

    /* 1MB Base64 */
    {
        size_t n = 1024 * 1024;
        char *big = (char *)malloc(n + 1);
        if (!big) {
            ok(0, "1MB 缓冲分配失败");
            return;
        }
        memset(big, 'A', n);
        big[n] = '\0';
        s.file_b64 = big;
        s.file_b64_len = n;
        ok(build(&s, &body, &len, err, sizeof(err)) == 0, "1MB Base64 能生成");
        ok(len == n + 24, "1MB body 长度 = n + 24");
        ok(body && body[len - 1] == '}', "1MB body 结尾是 }");
        ok(body && jl_is_json(body, len) == 1, "1MB body 仍是合法 JSON");
        free(body);
        body = NULL;
        free(big);
        s.file_b64 = "QUJD";
        s.file_b64_len = 4;
    }
}

static void t_errors(void)
{
    api_request_spec s;
    char err[512];
    char *body = NULL;
    size_t len = 0;

    base_spec(&s);
    s.file_type = 2;
    ok(build(&s, &body, &len, err, sizeof(err)) == -1, "fileType=2 被拒绝");
    ok(strstr(err, "fileType") != NULL, "错误信息点名 fileType");

    base_spec(&s);
    s.file_b64 = NULL;
    s.file_b64_len = 0;
    ok(build(&s, &body, &len, err, sizeof(err)) == -1, "没有文件被拒绝");

    base_spec(&s);
    s.file_b64 = "";
    s.file_b64_len = 0;
    ok(build(&s, &body, &len, err, sizeof(err)) == -1, "空文件被拒绝");

    ok(api_build_body(NULL, &body, &len, err, sizeof(err)) == -1, "空 spec 被拒绝");
}

static void t_filetype_map(void)
{
    int known = 1;

    ok(api_filetype_for_ext(".pdf", &known) == 0 && known == 1, ".pdf -> 0 且已识别");
    ok(api_filetype_for_ext(".png", &known) == 1 && known == 1, ".png -> 1 且已识别");
    ok(api_filetype_for_ext(".jpg", &known) == 1 && known == 1, ".jpg -> 1");
    ok(api_filetype_for_ext(".tif", &known) == 1 && known == 1, ".tif -> 1");
    ok(api_filetype_for_ext(".bmp", &known) == 1 && known == 1, ".bmp -> 1");
    ok(api_filetype_for_ext(".bin", &known) == 1 && known == 0, ".bin -> 1 但未识别");
    ok(api_filetype_for_ext(NULL, &known) == 1 && known == 0, "空扩展名 -> 1 未识别");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("==== apibody 单元测试 ====\n");

    printf("-- 参数表 --\n");     t_table();
    printf("-- 默认值 --\n");     t_defaults();
    printf("-- 精确 body --\n");  t_exact_body();
    printf("-- GET --\n");        t_get();
    printf("-- 全布尔 --\n");     t_all_bools();
    printf("-- 合法数值 --\n");   t_numbers_ok();
    printf("-- 非法数值 --\n");   t_numbers_bad();
    printf("-- 空值回落 --\n");   t_empty_value();
    printf("-- 数组形式 --\n");   t_lists();
    printf("-- 枚举 --\n");       t_enums();
    printf("-- 手写片段 --\n");   t_raw();
    printf("-- 转义与大 body --\n"); t_escape_and_size();
    printf("-- 错误路径 --\n");   t_errors();
    printf("-- fileType 映射 --\n"); t_filetype_map();

    printf("\n---- 结论 ----\n");
    printf("通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    if (g_fail == 0)
        printf("结果：ALL PASS\n");
    return g_fail ? 1 : 0;
}
