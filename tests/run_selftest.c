/*
 * run_selftest.c - 在开发机（macOS / Linux）上原生运行与 EXE 内完全相同的
 * 自检代码，用来确认「内嵌自检本身是通过的」。
 *
 * 编译运行：
 *     cc -std=c99 -O2 -o build/run_selftest tests/run_selftest.c \
 *        src/b64sha.c src/selftest.c && ./build/run_selftest
 */
#include "../src/selftest.h"

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    char *buf = (char *)malloc(65536);
    int fails;

    if (!buf) {
        fprintf(stderr, "内存不足\n");
        return 2;
    }
    fails = b64sha_selftest(buf, 65536);
    fputs(buf, stdout);
    free(buf);
    return fails == 0 ? 0 : 1;
}
