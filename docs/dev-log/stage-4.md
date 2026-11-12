# 階段 4：實作 `SYSINFO` / `ECHO` / `QUIT`

- **驗收標準**：回傳內容正確
- **驗收結果**：✅ 通過（SYSINFO 的 10 個欄位都和系統工具一致；ECHO、QUIT、未知指令、同一連線多個指令、CRLF、兩個 client 同時連線全部正確）

---

## 這次建立 / 修改的檔案

| 檔案 | 變更 |
|---|---|
| `server/server.c` | 新增 `build_sysinfo()`（`uname` + `sysinfo`）、`dispatch()`（指令解析）、`g_server_pid`；`handle_client` 改成「讀指令 → dispatch → 回一行」，收到 `QUIT` 回應後關閉連線；去掉行尾的 `\r` |

`client` 不需要修改：它本來就把命令列參數串成一行送出，再印出一行回覆。

| 指令 | 回應 |
|---|---|
| `SYSINFO` | `OK hostname=..;sysname=..;release=..;machine=..;uptime=..;totalram=..;freeram=..;procs=..;server_pid=..;child_pid=..` |
| `ECHO <text>` | `OK <text>` |
| `QUIT` | `OK bye`，然後 server 關閉連線 |
| 其他 | `ERR unknown command` |

### 驗收輸出

**T1. SYSINFO 與系統工具比對**

```text
$ ./client/client SYSINFO
OK hostname=REDACTED-HOSTNAME;sysname=Linux;release=6.6.114.1-microsoft-standard-WSL2;machine=x86_64;uptime=66;totalram=7997587456;freeram=7050219520;procs=312;server_pid=423;child_pid=426
```

| 欄位 | server 回傳 | 系統工具 | 一致 |
|---|---|---|---|
| hostname | `REDACTED-HOSTNAME` | `hostname` → `REDACTED-HOSTNAME` | ✅ （實測兩者相同；真實主機名稱已以標籤取代，見 stage-8.md 第 6 點） |
| sysname | `Linux` | `uname -s` | ✅ |
| release | `6.6.114.1-microsoft-standard-WSL2` | `uname -r` | ✅ |
| machine | `x86_64` | `uname -m` | ✅ |
| uptime | `66` | `/proc/uptime` → `65`（取樣時間差 1 秒內） | ✅ |
| totalram | `7997587456` | `free -b` total → `7997587456` | ✅ |
| freeram | `7050219520` | `free -b` free → `7048499200`（隨時在變動） | ✅ |
| procs | `312` | 見第 3 點說明 | ✅ |
| server_pid | `423` | 啟動 server 時 `$!` = 423 | ✅ |
| child_pid | `426` | server log：`child pid=426` | ✅ |

**T2. ECHO**

```text
ECHO hello world               → OK hello world
"ECHO   leading spaces kept"   → OK   leading spaces kept     ← "ECHO " 之後的空白原樣保留
"ECHO 你好，世界 !@#$%"         → OK 你好，世界 !@#$%
"ECHO "                        → OK                          ← 空字串
```

**T3. 未知指令**

```text
HELLO           → ERR unknown command
sysinfo         → ERR unknown command      ← 大小寫敏感
ECHO            → ERR unknown command      ← 少了空白與文字
SYSINFO extra   → ERR unknown command      ← 多了參數
```

**T4. 同一條連線送多個指令，QUIT 後連線關閉**

```text
一次寫入：ECHO one \n SYSINFO \n QUIT \n ECHO after-quit \n
收到：
OK one
OK hostname=REDACTED-HOSTNAME;sysname=Linux;release=6.6.114.1-microso...
OK bye
(cat ended -> server closed the connection)     ← "ECHO after-quit" 沒有被處理
```

**T5. CRLF 換行（telnet 的送法）**

```text
送出：'ECHO crlf\r\n' 'QUIT\r\n'
收到（cat -A，$ = \n）：OK crlf$  OK bye$      ← 回覆中沒有 \r，指令也正確辨識
```

**T6. 兩個 client 同時連線**

```text
A: server_pid=423;child_pid=472
B: server_pid=423;child_pid=473       ← 同一個 server，不同的子行程
```

Server log 節錄：

```text
[INFO ][463][server/server.c:156] 127.0.0.1:46288 cmd: ECHO one
[INFO ][463][server/server.c:156] 127.0.0.1:46288 cmd: SYSINFO
[INFO ][463][server/server.c:156] 127.0.0.1:46288 cmd: QUIT
[INFO ][463][server/server.c:167] 127.0.0.1:46288 requested QUIT
[INFO ][463][server/server.c:301] 127.0.0.1:46288 disconnected
```

---

## 為什麼這樣寫（含運作原理）

### 1. `uname()`：問核心「我是誰」

`uname(&u)` 會把核心的識別資訊填進 `struct utsname`：

| 欄位 | 意義 | 範例 |
|---|---|---|
| `nodename` | 主機名稱（網路上的名字） | `REDACTED-HOSTNAME` |
| `sysname` | 作業系統核心名稱 | `Linux` |
| `release` | 核心版本 | `6.6.114.1-microsoft-standard-WSL2` |
| `machine` | 硬體架構 | `x86_64` |

`uname -a` 指令本身就是呼叫這個系統呼叫。從 `release` 可以看出這是 WSL2：Windows 底下跑的是一個真正的 Linux 核心（由 Microsoft 編譯），不是模擬層，所以 `fork`、`sysinfo` 等功能都能正常使用。

### 2. `sysinfo()`：核心的即時統計（Linux 專用）

`sysinfo(&si)` 一次取得多項系統統計：

- `uptime`：開機到現在經過的秒數。
- `totalram` / `freeram`：總記憶體與**完全未使用**的記憶體。
- `procs`：目前的行程數。

**`mem_unit` 的陷阱**：`totalram` 的單位不是固定的 byte，而是 `mem_unit` bytes。在 x86_64 上通常是 1，但在某些 32 位元系統上，為了讓超過 4 GB 的記憶體能放進 32 位元的欄位裡，`mem_unit` 會大於 1。所以程式先把兩邊轉成 `unsigned long long` 再相乘，才不會在相乘時溢位。

**`freeram` 為什麼比想像中少**：Linux 會把閒置的記憶體拿去當**檔案快取**（page cache），所以 `freeram`（完全沒在用的）通常遠小於「可用」的記憶體。`free -b` 的 `free` 欄位就是同一個值，而 `available` 欄位才包含可以回收的快取。

**可攜性**：`sysinfo()` 是 Linux 專用的。macOS 要改用 `sysctl()`（CONTEXT.md B1）。

### 3. `procs` 和 `ps` 的數字不一樣？

`sysinfo` 的 `procs` 計算的是核心中所有的**執行緒**（Linux 核心不區分行程和執行緒，兩者都是 task），所以通常比 `ps -e | wc -l` 的行程數多。它和 `/proc/loadavg` 第四欄的分母是同一個數字。報告中可以用來說明：「從核心角度看，行程和執行緒都是排程的單位」。

### 4. `server_pid` 為什麼在 fork 之前記下，而不用 `getppid()`

```c
g_server_pid = getpid();   /* 父行程，在 fork 之前 */
...
pid_t pid = fork();        /* 子行程得到 g_server_pid 的複本 */
```

- **fork 會複製整個記憶體**，所以子行程自然擁有 `g_server_pid` 的值，不需要額外傳遞。log 等級（`g_log_level`）也是同樣的道理。
- 如果在子行程中呼叫 `getppid()`，平常也會得到相同的值。但如果父行程先結束（例如被 kill），子行程會被 init 收養，`getppid()` 就會變成 1（或 WSL 的 init 的 pid）。在 fork 前記下來，回報的才永遠是「當初接受這條連線的 server」。

`child_pid` 則是子行程自己的 `getpid()`。T6 中兩個同時在線的 client 拿到相同的 `server_pid`、不同的 `child_pid`，直接證明了「一個 client 一個子行程」的架構。

### 5. 指令解析：刻意保持嚴格又簡單

```c
if (strcmp(cmd, "SYSINFO") == 0)        { ... }   /* 完全相等 */
if (strncmp(cmd, "ECHO ", 5) == 0)      { ... }   /* 前 5 個字元是 "ECHO " */
if (strcmp(cmd, "QUIT") == 0)           { ... }
/* 其他 */                               ERR unknown command
```

- **完全比對**：`SYSINFO extra`、`sysinfo` 都視為未知指令。規則越少，行為越容易預測和測試，也不會超出 CONTEXT.md B3 的範圍。
- **ECHO 原樣回傳**：只去掉開頭的 `"ECHO "`（5 個字元），之後的內容（包括連續空白）全部保留。階段 5 會用 ECHO 來驗證加解密，所以必須是「送什麼就回什麼」。
- **`ECHO` 單獨出現**：不符合 `"ECHO "` 的前綴規則，所以是未知指令；`"ECHO "`（只有空白）則回傳 `OK `（空字串）。

### 6. 每則回應都是「單行」，而且長度可以保證

協定規定回應固定是單行（CONTEXT.md B4），client 只要呼叫一次 `recv_line` 就能讀到完整的回應：

- **ECHO 的長度**：輸入一行最多 1022 bytes 的內容，扣掉 `"ECHO "`（5 bytes）再加上 `"OK "`（3 bytes）只會更短，一定放得下。
- **SYSINFO 的長度**：大約 200 bytes。即使主機名稱很長（最多 64 字元），也遠小於 1024。但程式仍然檢查 `snprintf` 的回傳值：如果被截斷，就回傳 `ERR internal error`，而不是送出一則被切掉一半的回應。
- **不會出現 `\n`**：回應的內容都來自核心或 client 送來的一行，而 `send_line` 也會拒絕內含 `\n` 的字串，作為最後一道防線。

### 7. QUIT 的關閉流程

```text
client                          server（子行程）
  │ ── "QUIT\n" ──────────────▶ │ dispatch 回傳 quit=1
  │ ◀────────────── "OK bye\n" ─ │ send_line
  │                              │ return → close(connfd) → 送出 FIN
  │ ◀─────────────────── FIN ─── │ exit(0)
  │ recv 回傳 0（EOF）           │
```

**先送回應、再關閉**：如果先關閉，client 就收不到 `OK bye`。T4 中 QUIT 之後的 `ECHO after-quit` 已經在 server 的接收緩衝區裡了，但 server 不會再讀它。

> 小知識：如果 server 關閉時，接收緩衝區中還有**沒讀走**的資料，Linux 會送出 **RST** 而不是 FIN，告訴對方「有資料被丟掉了」。階段 8 用 tcpdump 觀察斷線時，可以拿 T4 的情況（QUIT 後還有資料）和一般的 QUIT 做對照。

### 8. 為什麼要去掉行尾的 `\r`

`telnet` 和部分 Windows 工具，按 Enter 時送出的是 `\r\n`（CRLF），而不是 `\n`。`recv_line` 以 `\n` 切行，所以 `\r` 會被留在指令的結尾，`"QUIT\r"` 就無法和 `"QUIT"` 比對成功。去掉行尾的單一 `\r` 之後，這類工具也能直接拿來測試（T5）。這只是在解析指令時做寬鬆處理，協定本身仍然以 `\n` 作為唯一的訊息邊界。

### 9. 這個階段仍刻意保留的限制

| 限制 | 在哪個階段處理 |
|---|---|
| 指令與回應都是明文，tcpdump 可以直接看到 | 階段 5：凱薩加密 |
| 殭屍行程累積 | 階段 6：R1 |
| 閒置的 client 永久佔用一個子行程 | 階段 6：R3 |
| client 一次只能送一個指令 | 不處理（超出 B3 範圍）；要送多個指令可以用 `nc` |
