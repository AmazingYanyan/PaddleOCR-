/*
 * selftest.h - 内置自检入口
 */
#ifndef SELFTEST_H
#define SELFTEST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 运行内置自检，把人类可读的报告写入 report（UTF-8）。
 * 返回失败项数：0 表示全部通过。
 */
int b64sha_selftest(char *report, size_t cch);

#ifdef __cplusplus
}
#endif

#endif /* SELFTEST_H */
