# 階段 2：`libutil.so` 加入 `send_all` / `recv_line` 與兩層 log

- **驗收標準**：`make` 與 `make DEBUG=1` 的輸出不同；`LOG_LEVEL` 生效
- **驗收結果**：✅ 通過（9 組測試，`make` 無任何警告；階段 1 的測試全部仍通過）

---

## 這次建立 / 修改的檔案

| 檔案 | 變更 |
|---|---|
| `include/util.h` | 新增 `MAX_LINE`、`send_all`、`recv_line`、`send_line`、log 介面與 `LOG_E` / `LOG_I` / `LOG_D` 巨集 |
| `lib/util.c` | 實作上述函式。`recv_line` 用 `MSG_PEEK`，不保存任何內部狀態；log 一次用 `write(2)` 寫出整行 |
| `server/server.c` | 改用 `recv_line` / `send_line`；所有錯誤改用 `LOG_E` 並附上 `strerror(errno)`；新增 `-v level` |
| `client/client.c` | 同上；回覆印到 stdout，log 寫到 stderr；新增 `-v level` |
| `Makefile` | 新增 `.cflags` 參數紀錄檔：`DEBUG` 改變時自動重編 |
| `.gitignore` | 加入 `.cflags` |
| `CONTEXT.md` | B6 註明 `send_all` 不加 `MSG_NOSIGNAL` 的理由 |

命令列：

```text
./server/server_naive [-p port] [-v level]
./client/client [-h host] [-p port] [-v level] <text...>
LOG_LEVEL=0|1|2 ./server/server_naive      # 環境變數；-v 優先
make DEBUG=1                                # 編譯期開啟 LOG_D
```

### 驗收輸出

**A/B. 編譯期開關：`make` 和 `make DEBUG=1` 產生的執行檔不同**

```text
$ make            → strings 搜尋 LOG_D 訊息：0 筆   .rodata 大小 399 bytes
$ make DEBUG=1    → 不用 clean 就自動重編（-DENABLE_DEBUG）
                  → strings 搜尋 LOG_D 訊息：1 筆   .rodata 大小 508 bytes
```

**C. 執行期等級（DEBUG 版）**

```text
LOG_LEVEL=0 → 沒有任何輸出（沒有錯誤發生）
LOG_LEVEL=1 →
[2026-11-11 22:55:43.254][INFO ][494][server/server.c:153] server (naive, libutil 0.2 (stage 2)) listening on port 9200
[2026-11-11 22:55:43.554][INFO ][494][server/server.c:175] 127.0.0.1:45776 connected (connfd=4)
[2026-11-11 22:55:43.554][INFO ][494][server/server.c:61] 127.0.0.1:45776 recv: hi
[2026-11-11 22:55:43.554][INFO ][494][server/server.c:177] 127.0.0.1:45776 disconnected
LOG_LEVEL=2 → 上面四行，再加上：
[...][DEBUG][500][server/server.c:154] debug log compiled in and enabled
[...][DEBUG][500][server/server.c:62] 127.0.0.1:45782 recv 3 bytes (incl. newline)
[...][DEBUG][500][server/server.c:68] 127.0.0.1:45782 sent reply
[...][DEBUG][500][server/server.c:49] 127.0.0.1:45782 sent FIN

LOG_LEVEL=2 + -v 0  → 沒有輸出（-v 優先於環境變數）
LOG_LEVEL=abc       → LOG_LEVEL=abc ignored (expected 0, 1 or 2)
```

**D/E. 兩層必須同時開啟，DEBUG 才會出現**

| 編譯 | 執行期等級 | DEBUG 行數 |
|---|---|---|
| `make DEBUG=1` | 2 | 4 |
| `make DEBUG=1` | 1（預設） | 0 |
| `make` | 2 | 0（程式碼根本不存在） |

**F. `recv_line` 的訊息邊界：分段送達、一次兩行、空行**

```text
client 依序寫入：'hel'  (等 0.2 秒)  'lo\nwor'  (等 0.2 秒)  'ld\n\nlast\n'
server 收到：  recv: hello / recv: world / recv:  / recv: last
client 收到回覆（cat -A，$ 代表 \n）： hello$ world$ $ last$
```

**G. 超長行（1100 bytes 且沒有 `\n`）**

```text
[ERROR][559][server/server.c:54] 127.0.0.1:39328 line too long (> 1023 bytes), closing
[INFO ][559][server/server.c:177] 127.0.0.1:39328 disconnected
```

**H. 階段 1 的測試**：基本 echo、中文與符號、client 端超長、server 不存在、非法 IP 都仍然通過；`./client ... > out.txt` 只會存到回覆，不會混進 log。

**I. `ldd`**：`server_robust` 和 `client` 都列出 `libutil.so`（階段 1 中 client 暫時沒連結 `.so`，現在恢復了）。

---

## 為什麼這樣寫（含運作原理）

### 1. `send_all`：為什麼 `send` 可能「送不完」

`send()` 並不是直接把資料丟上網路，而是**複製到核心的 socket 傳送緩衝區**，之後由 TCP 依對方的接收視窗與壅塞控制慢慢送出。

- 緩衝區空間足夠：全部複製進去，回傳 `len`。
- 緩衝區只剩一部分空間：只複製放得進去的部分，**回傳實際數量**（short write）。
- 緩衝區已滿：阻塞，等到有空間為止。
- 還沒複製任何資料就被信號打斷：回傳 -1，`errno == EINTR`。

所以 `send_all` 用迴圈處理：每次把指標往前移 `n`、剩餘量減去 `n`，直到全部送完；遇到 `EINTR` 就重試。本專案一行最多 1 KB，在本機幾乎不會發生 short write，但一個正確的網路程式**不能假設它不會發生**。

### 2. `send_all` 為什麼不加 `MSG_NOSIGNAL`

對一條已經被對方 RST 的連線呼叫 `send`，核心會送 **SIGPIPE** 給這個行程，而 SIGPIPE 的預設動作是**終止行程**。這正是 CONTEXT.md B9 的 R2。

如果 `send_all` 內建 `MSG_NOSIGNAL`，naive 版也會被保護到，階段 6 就做不出「有 / 無」對照了。因此：

- `libutil.so` 只編譯一次，行為保持中立：**不忽略 SIGPIPE**。
- 階段 6 的 robust 版在 server 內呼叫 `signal(SIGPIPE, SIG_IGN)`，由**呼叫端**決定。忽略之後，`send` 會改成回傳 -1 並設 `errno == EPIPE`，`send_all` 會把它回報給呼叫端，讓 server 記 log 並優雅地結束。

### 3. `recv_line`：用 `MSG_PEEK` 做出「無狀態」的帶緩衝讀取

階段 1 一次讀 1 byte 雖然正確，但每個 byte 都要一次系統呼叫（從使用者空間切到核心，再切回來）。一行 100 字就是 100 次。

常見的改法是「一次 recv 一大塊，存在緩衝區中再切行」，但會遇到一個問題：**多讀到的下一行開頭要存在哪裡？**

- 存在函式內的 `static` 緩衝區：這就成了隱藏狀態，同一個行程處理兩個 fd 時會混在一起。
- 交給呼叫端保存：介面要改成傳入一個 reader 結構，和 CONTEXT.md 定好的 `recv_line(fd, buf, maxlen)` 不符。

這次選擇用 **`MSG_PEEK`**：

```text
核心接收緩衝區： h e l l o \n w o r l d \n
                 └──── peek 看到全部，在第 6 個 byte 找到 '\n'
recv(6)  →  只取走 "hello\n"
核心剩下：       w o r l d \n           ← 留給下一次 recv_line
```

1. `recv(..., MSG_PEEK)`：把資料複製出來，但**不從核心緩衝區移除**。
2. 在複製出來的資料中找 `\n`。
3. 找到了，就真正 `recv` 恰好到 `\n` 為止的長度；沒找到，就把看到的部分全部取走，繼續等下一段資料。

結果是：多出來的資料一直留在核心中，函式本身完全沒有狀態，一行大約只需要 2 次系統呼叫。測試 F 就是在驗證，不論資料怎麼被切段（`hel` / `lo\nwor` / `ld\n\nlast\n`），切出來的行都是對的。

**回傳值包含 `\n`**，是為了區分「空行」（回傳 1）和「對方關閉」（回傳 0）。如果去掉 `\n` 再回傳長度，空行也會變成 0，兩者就分不出來了。測試 F 中的空行被正確地 echo 回來，就是證明。

**錯誤用 `errno` 區分**：`EMSGSIZE` 代表超長（階段 6 的 R4 會回傳 `ERR line too long`），`ECONNRESET` 代表一行讀到一半就斷線。

### 4. `send_line`：先組好一整塊再送

如果分兩次送（先送內容、再送 `\n`），會多一次系統呼叫，在 Nagle 演算法的作用下還可能被拆成兩個 TCP 封包，階段 8 抓封包時會很難看。所以先 `memcpy` 到暫存區，補上 `\n`，再一次 `send_all`。同時在**送出端**就擋下內含 `\n` 的字串，保護協定的訊息邊界。

### 5. 兩層 log：編譯期與執行期

```text
            ┌────────── 編譯期：make DEBUG=1 → -DENABLE_DEBUG ──────────┐
LOG_D(...)  │ 有定義 → log_write(LOG_DEBUG, __FILE__, __LINE__, ...)     │
            │ 沒定義 → ((void)0)        ← 程式碼與字串都不存在          │
            └────────────────────────────────────────────────────────────┘
                              │（有編進去才會到這裡）
            ┌────────── 執行期：LOG_LEVEL / -v ──────────────────────────┐
log_write   │ if (level > g_log_level) return;                           │
            └────────────────────────────────────────────────────────────┘
```

**編譯期（前置處理器）**：`#ifdef ENABLE_DEBUG` 在編譯之前就決定 `LOG_D(...)` 要展開成什麼。沒有定義時會變成 `((void)0)`，格式字串和參數都不會進入執行檔。測試 A/B 用 `strings` 找不到這段訊息，`.rodata`（唯讀資料段，字串常數存放的地方）也小了 109 bytes，證明它真的不存在。

- 好處：正式版本沒有任何額外負擔，也不會洩漏內部訊息。
- 為什麼寫成 `((void)0)` 而不是空的？因為 `if (x) LOG_D("..."); else ...` 這種寫法展開後仍是合法的單一敘述，`(void)` 也能避免「敘述沒有作用」的警告。

**執行期**：`g_log_level` 預設為 `LOG_INFO`。設定的優先順序是：程式內預設 < 環境變數 `LOG_LEVEL` < 命令列 `-v`。越接近「這一次執行」的設定越優先，所以 `log_init_from_env()` 先呼叫，`getopt` 處理 `-v` 再覆寫它。

**兩層必須同時開啟**（測試 D/E）。這正是 CONTEXT.md B7 的要求：即使編譯期開了 DEBUG，執行期等級低於 2 時仍不輸出。

### 6. 可變參數巨集與 `__FILE__` / `__LINE__`

```c
#define LOG_E(...) log_write(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
```

- `...` 與 `__VA_ARGS__`：把呼叫時的所有參數原封不動傳下去。
- `__FILE__` / `__LINE__`：前置處理器在**呼叫的位置**展開成檔名和行號。所以一定要寫成巨集；如果是函式，拿到的永遠是 `util.c` 自己的位置。
- `log_write` 內部用 `va_list` / `va_start` / `vsnprintf` 處理不定數量的參數。
- 宣告上的 `__attribute__((format(printf, 4, 5)))` 告訴 gcc：「第 4 個參數是 printf 格式字串，第 5 個之後是對應的值」。`LOG_I("%d", "str")` 這種型別錯誤就會在編譯期被抓到。

### 7. Log 格式與輸出方式

格式為 `[時間][等級][pid][檔案:行號] 訊息`：

- **時間到毫秒**：`clock_gettime` 加上 `localtime_r`。`_r` 版本把結果寫進呼叫端給的結構，不使用共用的 static 緩衝區。
- **pid**：階段 3 之後每個 client 由不同的子行程服務，沒有 pid 就分不出哪一行是哪個連線的。
- **為什麼用一次 `write(2)` 寫出整行，而不用 `fprintf(stderr)`？** 多個行程共用同一個 stderr 時，`fprintf` 可能把一行拆成好幾次 `write`，各行程的輸出就會穿插在一起。先在記憶體中組好完整的一行，再一次 `write`，每行就是一個不可分割的寫入。
- **保存並還原 `errno`**：呼叫端常常 log 完之後還要判斷 `errno`，而 log 內部的系統呼叫可能改掉它。
- **stdout 與 stderr 分流**：client 的回覆走 stdout，log 走 stderr，所以 `./client ... > out.txt` 只會存到回覆（測試 H），之後寫測試腳本比較容易處理。

### 8. Makefile：`.cflags` 參數紀錄檔

make 只看「檔案修改時間」。原始碼沒改的話，`make` 之後再 `make DEBUG=1`，make 會認為全部都是最新的而不重編，DEBUG 等於沒生效。這是很常見的陷阱。解法是：

```make
$(FLAGS_FILE): FORCE
	@echo '$(CFLAGS)' | cmp -s - $@ || echo '$(CFLAGS)' > $@
```

1. `FORCE` 是一個永遠「過期」的假目標，所以這條規則**每次都會執行**。
2. 用 `cmp` 比較這次的 `CFLAGS` 和上次存下來的內容，**不同才改寫檔案**，檔案的修改時間也才會更新。
3. 所有目標都相依於 `.cflags`，參數一改，全部自動重編；參數沒改就不動。

測試 B 中沒有 `make clean`，切換到 `DEBUG=1` 後就自動重編；切回 `make` 時，4 個目標也全部重編（測試 E）。

### 9. 錯誤處理一致化

所有系統呼叫都檢查回傳值，失敗時以 `LOG_E("...: %s", strerror(errno))` 記錄（CONTEXT.md Part D 第 3 點）。數字參數也從 `atoi` 改成 `strtol`，`atoi("abc")` 會默默回傳 0，`strtol` 則可以透過 `end` 指標判斷整個字串是不是合法的數字。

### 10. 這個階段仍刻意保留的限制

| 限制 | 在哪個階段處理 |
|---|---|
| 迭代式 server，一次只服務一個 client | 階段 3：`fork()` |
| 超長行只斷線，不回 `ERR line too long` | 階段 6：R4 |
| 沒有處理 SIGPIPE | 階段 6：R2 |
| 只會 echo，還沒有 `SYSINFO` / `ECHO` / `QUIT` | 階段 4 |
