/*
 * apitest.h - 第 4 个标签页「接口测试」的对外接口
 *
 * 这一页把"选文件 -> Base64 -> 拼 body -> 发请求 -> 解析 -> 存图/存文本"
 * 整条链路包起来。主窗口（main.c）只负责把它挂到第 4 个页签上，
 * 并把宿主能力（文件对话框、读写文件）注入进来，避免两个模块互相依赖。
 *
 * 线程模型：
 *   界面线程收集配置 -> CreateThread 执行 api_run() -> PostMessage(WM_API_DONE)
 *   回到界面线程渲染。worker 不碰任何窗口句柄，只投递一条消息。
 */
#ifndef APITEST_H
#define APITEST_H

#include <windows.h>
#include <commctrl.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* api_result 的完整定义在 apitest.c 内，这里只前置声明指针类型，
 * 因为 main.c 只负责把 WM_BATCH_ROW 的 lParam 原样转回来，不访问成员。 */
struct api_result_tag;
typedef struct api_result_tag api_result;

/* 请求完成的自定义消息：wParam = 代际号，lParam = api_result* */
#define WM_API_DONE    (WM_APP + 71)
/* 批量：单个文件返回（wParam = 行号，lParam = api_result*），以及整批结束 */
#define WM_BATCH_ROW   (WM_APP + 72)
#define WM_BATCH_DONE  (WM_APP + 73)

/* 本页控件 ID：独立号段，避开 main.c 的 100~199 */
enum {
    IDA_URL = 500,
    IDA_METHOD,
    IDA_SEND,
    IDA_CLEAR,
    IDA_FILE_EDIT,
    IDA_PICK_FILE,
    IDA_FILETYPE,
    IDA_PROXY,
    IDA_TIMEOUT,
    IDA_HINT,
    IDA_HOST,
    IDA_VSCROLL,
    IDA_META1,
    IDA_META2,
    IDA_RESP,
    IDA_COPY_RESP,
    IDA_OUTDIR,
    IDA_BROWSE,
    IDA_SAVE_IMG,
    IDA_SAVE_TXT,
    IDA_REDACT,
    IDA_PARAM_BASE = 600          /* 600 + i*2 = 复选框，601 + i*2 = 值输入框 */
};

/* 第 5 页（批量）控件 ID：独立号段 800~899，参数区用 900 起 */
enum {
    IDB_URL = 800,
    IDB_METHOD,
    IDB_SEND,
    IDB_STOP,
    IDB_CLEAR,
    IDB_DIR_EDIT,
    IDB_PICK_DIR,
    IDB_FILETYPE,
    IDB_PROXY,
    IDB_TIMEOUT,
    IDB_LIST,
    IDB_META,
    IDB_OUTDIR,
    IDB_BROWSE,
    IDB_SAVE_IMG,
    IDB_SAVE_TXT,
    IDB_REDACT,
    IDB_SEL_ALL,
    IDB_SEL_NONE,
    IDB_VSCROLL = 860,
    IDB_PARAM_BASE = 900
};

/* 宿主注入的能力：全部来自 main.c 里已验证过的实现 */
typedef struct {
    HINSTANCE inst;
    HWND (*mk)(HWND parent, const wchar_t *cls, const wchar_t *text,
               DWORD style, DWORD exstyle, int id, HFONT font);
    int  (*ds)(int v);
    int  (*line_h)(void);
    HFONT  font_ui;
    HFONT  font_mono;
    HBRUSH br_panel;

    /* 文件相关 */
    int  (*pick_file)(wchar_t *out, size_t cch);
    int  (*pick_save_txt)(wchar_t *out, size_t cch, const wchar_t *defname);
    int  (*pick_folder)(HWND owner, const wchar_t *title, wchar_t *out, size_t cch);
    int  (*read_file)(const wchar_t *path, unsigned char **out,
                      unsigned long long *len, wchar_t *err, size_t errc);
    int  (*write_file)(const wchar_t *path, const void *data, size_t len,
                       wchar_t *err, size_t errc);
    void (*unique_path)(wchar_t *path, size_t cch);
} api_ui_ctx;

/* 建立界面（在 build_ui 里对 g_panel[3] 调用一次） */
int  apitest_build(HWND panel, const api_ui_ctx *ctx);

/* 布局（面板坐标系，传 0,0,pw,ph） */
void apitest_layout(int px, int py, int pw, int ph);

/* 初始状态（默认值、提示文字） */
void apitest_init_state(void);

/* 关键控件是否都建出来了 */
int  apitest_ui_ok(void);

/* 切到本页时把焦点放到 URL 框 */
void apitest_focus_target(void);

/* 消息处理；返回 1 表示这条消息已经处理完 */
int  apitest_on_command(HWND hMain, WORD id, WORD code);

/* 滚动（由主窗口的 WM_VSCROLL / WM_MOUSEWHEEL 转来） */
void apitest_on_vscroll(WPARAM wp);
void apitest_on_wheel(int delta);
int  apitest_wants_wheel(void);       /* 鼠标是否落在本页滚动区里 */

/* 拖拽文件进来（本页只接受单个文件，直接设为源文件） */
void apitest_on_dropfiles(HDROP drop);

/* WM_API_DONE 处理 */
void apitest_on_done(UINT_PTR gen, LPARAM lp);

int  apitest_is_busy(void);           /* 有请求在跑 */
void apitest_on_close(void);          /* 窗口销毁前清理 */

/* 状态文字着色：返回 1 表示本页已经处理 */
int  apitest_ctlcolor(HWND ctl, HDC dc, LRESULT *out);

/* /diag 用的几何信息 */
void apitest_diag(wchar_t *buf, size_t cch);

/* /api-test 命令行自检；返回 1 表示已处理（程序应退出） */
int  handle_api_test(void);

/* ---------------- 第 5 页：批量接口测试 ---------------- */

int  batchtest_build(HWND panel, const api_ui_ctx *ctx);
void batchtest_layout(int px, int py, int pw, int ph);
void batchtest_init_state(void);
int  batchtest_ui_ok(void);

int  batchtest_on_command(HWND hMain, WORD id, WORD code);
int  batchtest_on_notify(NMHDR *nm);
void batchtest_on_vscroll(WPARAM wp);
void batchtest_on_wheel(int delta);
int  batchtest_wants_wheel(void);

void batchtest_on_row(int idx, api_result *res);
void batchtest_on_finished(void);
void batchtest_stop(void);
int  batchtest_is_busy(void);
void batchtest_on_close(void);
void batchtest_pick_dir(void);
void batchtest_diag(wchar_t *buf, size_t cch);

#ifdef __cplusplus
}
#endif

#endif /* APITEST_H */
