# 階段 6：強健性機制 R1、R2、R3（以 `ROBUST` 切換）

- **驗收標準**：三組對照可重現
- **驗收結果**：✅ 通過。`scripts/demo_robust.sh` 一次跑完 R1 / R2 / R3 的 naive 與 robust 對照；R2 連續跑 10 次都 10 次重現。回歸測試（指令功能、15 個 client 同時斷線、`-t 0`、持續活動的 client 不會被誤踢）全部通過

---

## 這次建立 / 修改的檔案

| 檔案 | 變更 |
|---|---|
| `server/server.c` | 在 `#ifdef ROBUST` 中加入：`sigchld_handler()` + `install_robust_signals()`（R1、R2）、`set_recv_timeout()`（R3）；新增 `-t timeout_sec`（預設 30，0 = 關閉，只有 robust 版會套用）；`handle_client` 能分辨 `EAGAIN`（R3 逾時）與 `EPIPE` / `ECONNRESET`（R2 對方已斷線） |
| `scripts/demo_robust.sh` | **新增**：強健性對照示範腳本，`./scripts/demo_robust.sh [r1\|r2\|r3\|all]` |
| `scripts/.gitkeep` | 刪除（目錄已有實際檔案） |

命令列：

```text
./server/server_robust [-p port] [-k shift] [-v level] [-t timeout_sec]
./scripts/demo_robust.sh [r1|r2|r3|all]       # PORT=9700 可覆寫
```

兩個版本的差異：

|  | `server_naive` | `server_robust`（`-DROBUST`） |
|---|---|---|
| R1 殭屍行程 | 不 wait | `SIGCHLD` handler 迴圈 `waitpid(-1, NULL, WNOHANG)`，`SA_RESTART` |
| R2 SIGPIPE | 預設處置（終止行程） | `SIGPIPE` → `SIG_IGN`，`send` 回傳 `EPIPE` 後優雅結束 |
| R3 閒置 client | 永久阻塞在 `recv` | `SO_RCVTIMEO`，逾時記 log 並關閉連線 |
| R5 `SO_REUSEADDR` | ✅（兩版都有，見 stage-1.md） | ✅ |
| R6 `EINTR` 重試 | ✅（`accept`、`recv_line`、`send_all`） | ✅ |

### 驗收輸出（`./scripts/demo_robust.sh all`）

**R1 殭屍行程**

```text
R1 殭屍行程 — server_naive
$ for i in 1..5: ./client/client ECHO hi-$i
$ ps --ppid 443 -o pid,ppid,stat,cmd
    PID    PPID STAT CMD
    446     443 Z+   [server_naive] <defunct>
    448     443 Z+   [server_naive] <defunct>
    450     443 Z+   [server_naive] <defunct>
    452     443 Z+   [server_naive] <defunct>
    454     443 Z+   [server_naive] <defunct>
  → 實際：zombie 數量 = 5

R1 殭屍行程 — server_robust
$ ps --ppid 460 -o pid,ppid,stat,cmd
    PID    PPID STAT CMD
  → 實際：zombie 數量 = 0
```

**R2 對已斷線的連線寫入（SIGPIPE）**

```text
R2 — server_naive
$ client 以一次 write 送出 5 行指令後立刻 close，不讀任何回覆
$ server log：
    [INFO ][428][server/server.c:438] 127.0.0.1:55588 connected (child pid=428, connfd=4)
    [INFO ][428][server/server.c:255] 127.0.0.1:55588 cmd: u
    [INFO ][428][server/server.c:255] 127.0.0.1:55588 cmd: u
                                                     ← 到這裡就沒了：沒有錯誤、沒有 disconnected
$ ps --ppid 426 -o pid,stat,cmd
    428     426 Z+   [server_naive] <defunct>
  → 子行程 428 的 exit_code = 13 → 被信號 13 終止（PIPE）

R2 — server_robust
    [INFO ][442][server/server.c:438] 127.0.0.1:55604 connected (child pid=442, connfd=4)
    [INFO ][442][server/server.c:255] 127.0.0.1:55604 cmd: u
    [INFO ][442][server/server.c:255] 127.0.0.1:55604 cmd: u
    [INFO ][442][server/server.c:266] 127.0.0.1:55604 peer gone while sending (Broken pipe), closing (R2)
    [INFO ][442][server/server.c:448] 127.0.0.1:55604 disconnected
$ ps --ppid 440 -o pid,stat,cmd
    PID    PPID STAT CMD                              ← 正常結束並被回收
```

（`cmd: u` 是因為腳本送的是 `x`，經 k=3 解密後變成 `u`，屬於未知指令。這裡的重點不在指令內容，而是 server 回覆時對方已經斷線。）

可靠度：連續執行 10 次，10 次 naive 都被 SIGPIPE 終止，10 次 robust 都記錄 `Broken pipe` 並優雅結束。

**R3 閒置 client（`-t 3`）**

```text
R3 — server_naive -t 3
$ ps（剛連上）       503  501 S+  ./server/server_naive -p 9700 -t 3
$ sleep 5
$ ps（5 秒後）       503  501 S+  ./server/server_naive -p 9700 -t 3     ← 還在，永遠不會結束
$ server log：  127.0.0.1:53174 connected (child pid=503, connfd=4)

R3 — server_robust -t 3
$ ps（剛連上）       516  514 S+  ./server/server_robust -p 9700 -t 3
$ sleep 5
$ ps（5 秒後）       （空）                                               ← 已結束並被回收
$ server log：
    127.0.0.1:53176 connected (child pid=516, connfd=4)
    127.0.0.1:53176 idle for 3 s, closing (R3)
    127.0.0.1:53176 disconnected
```

**回歸測試（robust）**

```text
ECHO / SYSINFO / 未知指令 / QUIT                        → 結果與階段 5 相同
15 個 client 同時在線 → 同一瞬間全部關閉                → children=0 zombies=0
之後再連線                                               → OK still alive（accept 沒有被 SIGCHLD 打壞）
robust log 中的 ERROR 行數                               → 0
-t 0                                                     → 1.5 秒後子行程仍在（逾時已關閉）
-t 2，client 每 1.5 秒送一次指令，共 3 次               → 3 次都有回覆，沒有被判定為閒置
naive 加上 -t 5                                          → 啟動 log：(-t ignored in naive build)
make DEBUG=1 / make                                      → 皆無警告
```

---

## 為什麼這樣寫（含運作原理）

### 0. 信號（signal）是什麼

信號是核心送給行程的**非同步通知**。行程收到信號時，核心會打斷它正在做的事，依照它對這個信號的**處置（disposition）**來處理：

| 處置 | 效果 |
|---|---|
| 預設（`SIG_DFL`） | 每個信號有自己的預設動作：`SIGPIPE`、`SIGINT` 是**終止行程**；`SIGCHLD` 是**忽略** |
| 忽略（`SIG_IGN`） | 核心直接丟掉這個信號 |
| 自訂 handler | 核心暫停主程式，跳去執行 handler，執行完再回到原本的地方 |

**`fork` 與信號處置**：子行程會繼承父行程**所有**的信號處置。所以 robust 版只要在第一次 `fork` 之前設定一次，之後每個子行程都會有同樣的設定。

### 1. R1：殭屍行程與 SIGCHLD

殭屍是怎麼產生的，已經在 stage-3.md 第 4 點說明過：子行程結束後，核心會保留它的結束狀態，直到父行程 `wait` 為止。子行程結束時，核心會送一個 **`SIGCHLD`** 給父行程，但它的預設動作是「忽略」，所以 naive 版完全不知道有子行程結束了。

robust 版安裝了一個 handler：

```c
static void sigchld_handler(int signo)
{
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    errno = saved_errno;
}
```

每一個細節都有原因：

| 寫法 | 為什麼 |
|---|---|
| `waitpid(-1, ...)` | `-1` = 任何一個子行程，不限定特定的 pid |
| **`while` 迴圈** | **標準信號不排隊**。handler 執行期間又有 3 個子行程結束，核心只會記錄「有一個待處理的 SIGCHLD」，handler 只會再被呼叫一次。如果只呼叫一次 `waitpid`，就會漏收 2 個。回歸測試中 15 個 client 同時斷線，殭屍仍為 0，就是在驗證這一點 |
| `WNOHANG` | 沒有已結束的子行程時，**立刻回傳 0**，而不是阻塞等待。handler 裡絕不能卡住，否則父行程就再也回不到 `accept` |
| 保存 / 還原 `errno` | handler 可能在主程式「剛呼叫完系統呼叫、還沒讀 `errno`」的時候插進來。迴圈的最後一次 `waitpid` 會回傳 -1 並把 `errno` 設成 `ECHILD`，不還原的話，主程式會讀到錯誤的 `errno` |
| **不在 handler 裡寫 log** | `log_write` 用到 `snprintf`、`localtime_r`，它們不是 **async-signal-safe**。如果主程式正在執行同一個函式（例如持有 libc 內部的鎖）時被信號打斷，handler 再呼叫一次就可能死結或弄亂資料。handler 內只能呼叫 async-signal-safe 的函式（`waitpid`、`write` 等，見 `man 7 signal-safety`） |

**`sigaction` 而不是 `signal()`**：`signal()` 在不同系統上的行為不一致（有些系統執行一次 handler 後會重設成預設處置，也不一定會自動重啟系統呼叫）。`sigaction` 可以明確指定以下行為：

- **`SA_RESTART`**：父行程大部分時間都阻塞在 `accept`。收到 SIGCHLD、執行完 handler 之後，`accept` 如果直接回傳 -1 / `EINTR`，就要靠程式自己重試。`SA_RESTART` 會讓核心**自動重新呼叫** `accept`。程式中 `accept` 的 `EINTR` 重試是第二層保險（R6）。
- **`SA_NOCLDSTOP`**：子行程只是被暫停（例如 `SIGSTOP`）時不要送 SIGCHLD，只在真正結束時才送。

### 2. R2：SIGPIPE

**什麼時候會收到 SIGPIPE**：對一條「對方已經送過 **RST**」的連線寫入時，核心會送 `SIGPIPE` 給寫入的行程，而它的預設動作是**終止行程**。

**為什麼要「第二次」寫入才會觸發**：這是 TCP 的半關閉（half-close）特性。

```text
client                                server（子行程）
  │ ── "x\nx\nx\nx\nx\n" ───────────▶ │   （一次寫入 5 行）
  │ close() ── FIN ─────────────────▶ │   client 已經不會再讀了
  │                                    │ recv 第 1 行 → send 回覆 → ✅ 成功（只是放進核心緩衝區）
  │ ◀──────────────────── 回覆資料 ── │
  │ 核心：socket 已關閉，還有資料送來  │
  │ ── RST ─────────────────────────▶ │   「這條連線已經不存在了」
  │                                    │ recv 第 2 行 → send 回覆 → 💥 SIGPIPE
```

- 收到 **FIN** 只代表「對方不會再**送**資料了」，並不代表對方不會再**收**資料。所以 server 的第一次 `send` 是合法的，會成功。
- client 的 socket 已經完全關閉，收到資料就會回 **RST**。
- server 的第二次 `send`，寫入的對象是一條已被 RST 的連線 → **SIGPIPE**。

**naive 版的後果**：子行程在 `send` 裡面被殺掉，`send_line` **根本不會回傳**，所以：

- 沒有任何錯誤 log，也沒有 `disconnected`（demo 的 log 停在第二個 `cmd: u`）。
- 來不及做任何清理。這個專案的清理只有 `close`，但在真實的 server 中，可能是寫到一半的檔案、沒有釋放的鎖、沒有 commit 的交易。
- 從外部看，這個 client 就這樣**無聲無息地消失了**，非常難除錯。

demo 中讀取殭屍的 `/proc/<pid>/stat` 最後一欄 `exit_code`（編碼方式和 `waitpid` 的 status 相同，低 7 位元是終止它的信號編號），得到 `13`，`kill -l 13` = `PIPE`。這直接證明了子行程是被 SIGPIPE 殺掉的。

**robust 版**：

```c
ign.sa_handler = SIG_IGN;
sigaction(SIGPIPE, &ign, NULL);
```

忽略之後，同樣的 `send` 會改成**回傳 -1，並設 `errno = EPIPE`**（Broken pipe）。`send_all` 把錯誤往上回報，`handle_client` 記下 `peer gone while sending (Broken pipe), closing (R2)`，然後正常 `return` → `close(connfd)` → `exit(0)`。

**為什麼選擇在 server 忽略 SIGPIPE，而不是在 `send_all` 加 `MSG_NOSIGNAL`**（stage-2.md 第 2 點）：`libutil.so` 只編譯一次，naive 和 robust 共用同一份。如果在 `.so` 裡加 `MSG_NOSIGNAL`，naive 版也會被保護到，就做不出這組對照了。

**同時處理 `ECONNRESET`**：如果 RST 在 server 第一次寫入**之前**就已經到達（例如 client 用 `SO_LINGER` 為 0 的方式強制關閉），`send` 會先回傳一次 `ECONNRESET`，而不是 SIGPIPE。兩個版本都會把它當成「對方已斷線」正常處理。開發過程中用 Python 探測時就遇過這種情況，所以 demo 腳本刻意用「一次寫入、立刻 close」的方式，讓 client 送出的是 FIN 而不是 RST。

### 3. R3：閒置 client 與 SO_RCVTIMEO

**naive 版**：client 連上後什麼都不送，子行程就永遠阻塞在 `recv`。每個這樣的連線都佔用一個行程、一個 fd 和一些記憶體。攻擊者只要開大量連線、什麼都不送，就能把 server 的資源耗盡（這是 **Slowloris** 類型攻擊的原理）。

**robust 版**：

```c
struct timeval tv = { .tv_sec = sec, .tv_usec = 0 };
setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
```

設定之後，`recv` 如果 `sec` 秒內**完全沒有收到資料**，就會回傳 -1，並設 `errno = EAGAIN`（也就是 `EWOULDBLOCK`）。`recv_line` 把這個錯誤原樣往上回報，`handle_client` 記下 `idle for 3 s, closing (R3)` 並關閉連線。

| 細節 | 說明 |
|---|---|
| 設定在 `connfd` 上，而且在**子行程**中設定 | 逾時是 socket 的屬性，每條連線各自計時，互不影響 |
| 「閒置」的定義 | 是**每一次 `recv`** 的等待時間，不是整條連線的總時間。回歸測試中 client 每 1.5 秒送一次指令（`-t 2`），雖然總共連線了 4.5 秒，也不會被踢掉 |
| `MSG_PEEK` 也會遵守逾時 | `recv_line` 的 peek 和真正讀取都會受到同一個逾時限制 |
| 設定失敗不致命 | 只是少了逾時保護，連線仍然可以正常服務，所以記 log 之後繼續執行 |
| `-t 0` | 不設定逾時，等同於 naive 的行為（可以拿來對照） |

**與信號的交互作用**：`man 7 signal` 中提到，**設定了逾時的 socket 操作，被信號打斷時「不會」被 `SA_RESTART` 自動重啟**，一律回傳 `EINTR`。`recv_line` 本來就會在 `EINTR` 時重試，所以沒有問題。這也說明了為什麼 R6（`EINTR` 重試）在兩個版本中都要保留：`SA_RESTART` 並不能涵蓋所有情況。

### 4. 同一份原始碼、兩個執行檔：條件編譯

```c
#ifdef ROBUST
    if (install_robust_signals() < 0) { ... }
#endif
```

`Makefile` 的 `server_robust` 規則多加了 `-DROBUST`（stage-0.md 第 5 點）。`#ifdef` 在**前置處理階段**就決定要保留哪些程式碼，所以：

- naive 版的執行檔中**完全沒有** `sigchld_handler`、`set_recv_timeout` 的程式碼。
- 兩個版本只差在強健性機制，其他邏輯完全相同，對照實驗的**控制變因**很乾淨。
- 可以用 `nm server/server_naive | grep sigchld` 驗證：naive 版找不到這個符號。

### 5. demo 腳本的設計

| 設計 | 原因 |
|---|---|
| 只用 bash 內建的 `/dev/tcp` 和 `ps` | 不需要 root，也不需要額外安裝 `nc` 或 Python，換一台乾淨的電腦也能執行（CONTEXT.md Part D 第 8 點） |
| 每段都印出指令、預期現象與實際觀察 | 方便截圖放進報告（CONTEXT.md B9：「指令 + 預期現象 + 截圖」） |
| `trap ... EXIT` | 腳本中途被 Ctrl+C 中斷，也會把 server 關掉、清除暫存 log |
| 可以單獨執行 `r1` / `r2` / `r3` | 錄影或截圖時，一次只示範一組 |
| R3 用 `-t 3` | 30 秒的預設值太長，不適合現場 demo |

### 6. 這個階段刻意沒有做的部分

| 項目 | 狀態 |
|---|---|
| R4 超長輸入回覆 `ERR line too long` | 選配，未實作。兩個版本的 `recv_line` 都已經限制長度、不會溢位（stage-2.md），只是直接斷線、沒有回覆錯誤訊息。所以 naive 版沒有「緩衝區溢位」的問題可以拿來對照 |
| SIGINT 優雅關閉 | 不在 B9 範圍內；以 Ctrl+C 的預設動作結束 server |
| 壓測與 10+ 併發證明 | 階段 7 |
