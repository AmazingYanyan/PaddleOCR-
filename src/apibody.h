/*
 * apibody.h - 接口测试的参数表与请求体拼接
 *
 * 这里只负责"把界面上的开关状态变成一段合法的 JSON 请求体"，不涉及任何网络、
 * 文件、窗口操作，因此可以在 macOS / Linux 上直接单元测试（tests/test_apibody.c）。
 *
 * 参数分三类：
 *   AP_BOOL  值为 true/false，勾选才出现（关闭时整个字段省略，而不是发 false）
 *   AP_NUM   数字；layoutUnclipRatio 额外允许 [a,b] 形式
 *   AP_ENUM  枚举，从固定候选里选，避免手写错大小写
 *   AP_RAW   手写 JSON 片段（数组或对象），插入前做括号配平校验
 */
#ifndef APIBODY_H
#define APIBODY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define API_MAX_PARAMS   32
#define API_VALUE_MAX    160
#define API_MAX_BODY     ((size_t)48 * 1024 * 1024)   /* 请求体上限 48MB */

typedef enum {
    AP_BOOL = 0,
    AP_NUM,
    AP_ENUM,
    AP_RAW
} api_ptype;

/* 参数分组，界面据此分区显示 */
typedef enum {
    AP_GROUP_SWITCH = 0,   /* 功能开关 */
    AP_GROUP_VALUE,        /* 数值 / 枚举 */
    AP_GROUP_RAW           /* 手写 JSON 片段 */
} api_group;

typedef struct {
    const char *name;        /* JSON 字段名（原样写入 body） */
    api_ptype   type;
    api_group   group;
    const char *def_value;   /* 默认值文本；AP_BOOL 忽略此项 */
    const char *opts;        /* AP_ENUM 的候选值，形如 "large|small|union" */
    const char *tip;         /* 中文说明（界面提示 / 文档） */
} api_param_def;

typedef struct {
    int  on;                        /* 1 = 勾选，参与拼接 */
    char value[API_VALUE_MAX];      /* 数值 / 枚举 / 片段文本（AP_BOOL 忽略） */
} api_param_state;

typedef struct {
    int         method_get;         /* 1 = GET（按约定不发 body） */
    int         file_type;          /* 0 = PDF，1 = 图片/TIFF */
    const char *file_b64;           /* 文件内容的 Base64（不带引号） */
    size_t      file_b64_len;
    api_param_state params[API_MAX_PARAMS];
} api_request_spec;

/* ---------------- 参数表 ---------------- */

size_t               api_param_count(void);
const api_param_def *api_param_at(size_t i);

/* 把 params 全部复位成默认状态（只有 useLayoutDetection 默认勾选） */
void api_params_default(api_param_state *params);

/* ---------------- 请求体 ---------------- */

/*
 * 拼出完整的 JSON 请求体。
 *   成功返回 0，*out 为 malloc 出来的字符串（需 free），*out_len 为长度。
 *   失败返回 -1，err 里写明是哪个参数的问题（便于界面直接提示）。
 *   method_get 非 0 时不生成 body（*out 为 NULL，*out_len 为 0），返回 0。
 */
int api_build_body(const api_request_spec *spec, char **out, size_t *out_len,
                   char *err, size_t errc);

/* ---------------- 文件类型映射 ---------------- */

/*
 * 按扩展名决定 fileType：PDF -> 0，图片/TIFF -> 1。
 * known 非空时回写"是否明确识别"（.bin 等未知类型按 1 处理但 known=0）。
 */
int api_filetype_for_ext(const char *ext, int *known);

/* 内置自检（目标机上跑，确认参数表与拼接逻辑没被编坏） */
int api_selftest(char *report, size_t cch);

#ifdef __cplusplus
}
#endif

#endif /* APIBODY_H */
