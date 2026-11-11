#ifndef UTIL_H
#define UTIL_H

/*
 * libutil.so 的公開介面（CONTEXT.md B6）。
 *
 * 階段 2：網路 I/O（send_all / recv_line / send_line）＋ 兩層 log。
 * 階段 5 會再加入 caesar_encrypt / caesar_decrypt。
 */

#include <stddef.h>      /* size_t */
#include <sys/types.h>   /* ssize_t */

#define MAX_LINE 1024    /* 單行上限，含結尾 '\n'（CONTEXT.md B4） */

/* 回傳函式庫版本字串（server 啟動時印出，方便確認載入的是哪一版 .so） */
const char *util_version(void);

/* --- 網路 I/O --- */

/* 把 buf 的 len 個 bytes 全部送出。
 * 處理部分寫入（short write）與 EINTR。
 * 回傳：len = 成功；-1 = 錯誤（errno 保留系統呼叫的值）。
 *
 * 注意：這裡「不」使用 MSG_NOSIGNAL。對已關閉的連線寫入時是否被 SIGPIPE 終止，
 * 交給呼叫端決定（robust 版在 server 內忽略 SIGPIPE）。這樣 naive 版才能在
 * 階段 6 重現 R2 的問題，做出「有 / 無」對照。 */
ssize_t send_all(int fd, const void *buf, size_t len);

/* 讀一行（到 '\n' 為止，含 '\n'），結果以 '\0' 結尾。
 * 回傳：
 *   > 0  行長度（含 '\n'，所以空行也至少是 1）
 *   0    對方正常關閉，且沒有讀到任何資料
 *   -1   錯誤；errno == EMSGSIZE 代表超過 maxlen-1 仍沒有 '\n'（超長），
 *        errno == ECONNRESET 代表一行讀到一半對方就關閉 */
ssize_t recv_line(int fd, char *buf, size_t maxlen);

/* 送出一行：自動在 line 後面補 '\n'。line 本身不可含 '\n'。
 * 回傳：0 = 成功；-1 = 錯誤（errno == EMSGSIZE 代表過長或內含 '\n'）。 */
int send_line(int fd, const char *line);

/* --- Log --- */

enum { LOG_ERROR = 0, LOG_INFO = 1, LOG_DEBUG = 2 };

/* 設定執行期 log 等級；等級數字大於此值的訊息不輸出。預設 LOG_INFO。 */
void log_set_level(int level);

/* 從環境變數 LOG_LEVEL（0|1|2）讀取等級；未設定或格式錯誤則維持原值。 */
void log_init_from_env(void);

/* 輸出一行 log 到 stderr：[時間][等級][pid][檔案:行號] 訊息
 * 一般不直接呼叫，用下面的 LOG_E / LOG_I / LOG_D 巨集。 */
void log_write(int level, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));   /* 讓 gcc 檢查格式字串與參數型別 */

#define LOG_E(...) log_write(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_I(...) log_write(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)

/* 編譯期開關：沒有 -DENABLE_DEBUG 時，LOG_D 在前置處理階段就被換成 ((void)0)，
 * 連格式字串都不會出現在執行檔裡（可用 strings 驗證）。 */
#ifdef ENABLE_DEBUG
#define LOG_D(...) log_write(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#else
#define LOG_D(...) ((void)0)
#endif

#endif /* UTIL_H */
