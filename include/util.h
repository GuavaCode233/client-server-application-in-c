#ifndef UTIL_H
#define UTIL_H

/*
 * libutil.so 的公開介面。
 *
 * 階段 0：只放一個 util_version()，目的是「確認 .so 能被編譯、連結、載入」。
 * 之後的階段會依 CONTEXT.md B6 逐步加入 Caesar、send_all/recv_line、log 等函式。
 */

/* 回傳函式庫版本字串。
 * 為什麼階段 0 就需要一個真正的函式？
 * Ubuntu 的 gcc 預設帶 --as-needed：執行檔若沒呼叫 .so 裡任何符號，
 * 連結器會把 -lutil 丟掉，ldd 就看不到 libutil.so，無法驗收。 */
const char *util_version(void);

#endif /* UTIL_H */
