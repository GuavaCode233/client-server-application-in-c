# 階段 3：用 `fork()` 處理多個 client（刻意不處理 SIGCHLD）

- **驗收標準**：兩個 client 同時連線；觀察到 zombie
- **驗收結果**：✅ 通過（A、B 同時在線並交錯對話，第三個 client 也能同時被服務；client 斷線後出現 `Z <defunct>`，而且會不斷累積）

---

## 這次建立 / 修改的檔案

| 檔案 | 變更 |
|---|---|
| `server/server.c` | `accept` 之後 `fork()`：子行程 `close(listenfd)` → `handle_client` → `close(connfd)` → `exit(0)`；父行程 `close(connfd)` 後立刻回到 `accept`。`fork` 失敗時記 log、關掉該連線，server 繼續運作 |

`client`、`libutil.so` 沒有變動。

### 驗收輸出

**T1. 兩個 client 同時連線，交錯對話**

```text
A、B 用 /dev/tcp 建立連線後都不斷線，交錯送資料：
B got: from-B-1
A got: from-A-1
B got: from-B-2
--- A、B 仍在線時，第三個 client C：
from-C            ← 立刻得到回覆（階段 1 時這裡會卡住）
```

Server log（第三欄是 pid）：

```text
[...][INFO ][423][server.c:156] server (naive, ...) listening on port 9300     ← 父行程
[...][INFO ][425][server.c:198] 127.0.0.1:59128 connected (child pid=425, connfd=4)   ← A
[...][INFO ][427][server.c:198] 127.0.0.1:59144 connected (child pid=427, connfd=4)   ← B
[...][INFO ][427][server.c:64]  127.0.0.1:59144 recv: from-B-1
[...][INFO ][425][server.c:64]  127.0.0.1:59128 recv: from-A-1
[...][INFO ][427][server.c:64]  127.0.0.1:59144 recv: from-B-2
[...][INFO ][429][server.c:198] 127.0.0.1:59160 connected (child pid=429, connfd=4)   ← C
```

每個 client 由不同 pid 的子行程服務。三個子行程的 `connfd` **都是 4**，因為 fd 是「每個行程各自一張表」的索引。

**T2. A、B 在線時的行程樹**

```text
    PID    PPID STAT CMD
    425     423 S+   ./server/server_naive -p 9300     ← A 的子行程（S = sleeping，阻塞在 recv）
    427     423 S+   ./server/server_naive -p 9300     ← B 的子行程
    429     423 Z+   [server_naive] <defunct>           ← C 已經結束 → 殭屍
    423     407 S+   ./server/server_naive -p 9300     ← 父行程（阻塞在 accept）
```

**T3. fork 後各自關掉不需要的 fd**

```text
parent 481 socket fds:
  fd 3 -> socket:[13687]        ← 只剩 listenfd
child  484 socket fds:
  fd 4 -> socket:[13688]        ← 只剩 connfd（fd 3 已關閉）

$ ss -tanp '( sport = :9301 )'
LISTEN 0.0.0.0:9301                      pid=481,fd=3    ← 監聽 socket 只屬於父行程
ESTAB  127.0.0.1:9301 127.0.0.1:49682    pid=484,fd=4    ← 連線 socket 只屬於子行程
```

**T4 / T5. Client 斷線後的殭屍會累積**

```text
A、B 斷線後：
    425     423 Z+   [server_naive] <defunct>
    427     423 Z+   [server_naive] <defunct>
    429     423 Z+   [server_naive] <defunct>
再連 3 個 client → zombie count: 6
```

**T6. 結束父行程 → 殭屍被 init 收養並回收**

```text
kill 父行程後：former zombies still exist: (empty = all reaped)
```

---

## 為什麼這樣寫（含運作原理）

### 1. `fork()`：一次呼叫，兩次回傳

`fork()` 會建立一個**幾乎完全相同**的新行程（子行程），包括：記憶體內容（以 copy-on-write 方式共享，寫入時才真正複製）、程式執行到的位置、**fd 表**、信號處置方式。

兩個行程都從 `fork()` 回傳的地方繼續執行，只有回傳值不同：

| 回傳值 | 在哪個行程 | 意義 |
|---|---|---|
| `0` | 子行程 | 「我是子行程」；可用 `getpid()` 查自己的 pid，`getppid()` 查父行程 |
| `> 0` | 父行程 | 新子行程的 pid |
| `-1` | 父行程（沒有建立子行程） | 失敗，例如行程數到達上限（`EAGAIN`） |

所以 `if (pid == 0) { ... exit(0); }` 這段**只有子行程會進去**。最後的 `exit(0)` 不能省略：如果省略，子行程會掉回 `for` 迴圈去呼叫 `accept`，可是它已經關掉 `listenfd` 了，只會不斷出錯。

### 2. fork 後 fd 的處理：為什麼父、子都要 close

```text
fork 之前（父）              fork 之後
fd 3 ─┐                      父 fd 3 ─┐            子 fd 3 ─┐
      ▼                               ▼                      ▼
   [listen socket] refcnt=1        [listen socket] refcnt=2  ← 同一個核心物件
fd 4 ─┐                      父 fd 4 ─┐            子 fd 4 ─┐
      ▼                               ▼                      ▼
   [conn socket]   refcnt=1        [conn socket]   refcnt=2
```

fork 複製的是 **fd 表**（行程裡的索引），不是 socket 本身。兩邊的 fd 3 指向**同一個**核心 socket 物件，物件內有**參考計數**。`close(fd)` 只是讓計數減 1，**計數降到 0 時**，核心才真的關閉 socket（對 TCP 而言就是送出 FIN）。

| 誰該關什麼 | 不關的後果 |
|---|---|
| **父關 `connfd`** | ① 子行程結束時計數只降到 1，**連線不會關閉**，client 永遠等不到 EOF；② 父行程每接一個連線就洩漏一個 fd，達到上限（`ulimit -n`，通常 1024）之後 `accept` 會失敗並回傳 `EMFILE` |
| **子關 `listenfd`** | ① 每個子行程都握著監聽 socket，父行程結束後 port 仍被佔住，新的 server 無法 bind；② 理論上子行程也能 `accept`，可能「搶走」本該由父行程處理的連線 |

T3 的 `/proc/<pid>/fd` 和 `ss -p` 證明處理正確：父行程只剩 fd 3（listen），子行程只剩 fd 4（conn）。

### 3. 為什麼 fork 之後 log 不會重複或錯亂

用 `fork` 時有一個經典陷阱：如果父行程的 `printf` 還有資料留在 **stdio 緩衝區**（使用者空間）裡，fork 會把這塊緩衝區一起複製過去，之後兩個行程各自 flush，同一段文字就會**印出兩次**。

這個專案沒有這個問題，因為 `log_write` 是一次 `write(2)` 直接交給核心，不經過 stdio 緩衝區（見 stage-2.md 第 7 點）。從 log 也看得出來，多個子行程同時寫 log，每一行仍然完整，沒有互相穿插。

### 4. 殭屍行程（zombie）是怎麼產生的

行程結束時（`exit`），核心會釋放它的記憶體、關閉它的 fd，但會**保留一小筆紀錄**：pid、結束狀態（exit code 或被哪個信號殺掉）、資源使用統計。這筆紀錄要等**父行程呼叫 `wait()` / `waitpid()` 取走**之後才會刪除。

```text
子行程 exit(0)
    │
    ▼
核心：釋放記憶體、關閉 fd，保留 {pid, 狀態}   → ps 顯示 Z <defunct>
    │  同時送 SIGCHLD 給父行程（預設動作：忽略）
    ▼
父行程 waitpid() 取走狀態  → 紀錄刪除，pid 可以重新使用
```

這個階段的父行程只會 `accept` + `fork`，**從來不 `wait`**，所以每個結束的子行程都會變成殭屍（T4、T5，數量只增不減）。

**殭屍的危害**：它不佔記憶體和 CPU，但**佔用一個 pid 和一個行程表項目**。系統的 pid 數量和每個使用者能有的行程數（`ulimit -u`）都有上限，殭屍累積到上限時，`fork()` 會失敗並回傳 `EAGAIN`，server 就無法再服務新的 client。長時間運行的 server 一定要處理這個問題。

### 5. 為什麼結束父行程後殭屍就消失了（T6）

父行程結束時，它的子行程會變成**孤兒**，由 init（pid 1，在 WSL 中是 WSL 的 init）**收養**。init 的工作之一就是不斷 `wait` 回收被收養的子行程，所以殭屍立刻就被清掉了。

但「關掉 server 重開」不是解法，因為 server 的工作就是一直運行。

### 6. 為什麼這個階段刻意不處理 SIGCHLD

CONTEXT.md B9 的 R1 要求做出「無機制 / 有機制」對照。這個階段先讓 naive 的行為（殭屍累積）出現並記錄下來，階段 6 再在 `ROBUST` 版本加上：

```c
/* 預告：階段 6 R1 */
struct sigaction sa = { .sa_handler = sigchld_handler, .sa_flags = SA_RESTART };
sigaction(SIGCHLD, &sa, NULL);
/* handler 內：while (waitpid(-1, NULL, WNOHANG) > 0); */
```

這裡先記下兩個要點，階段 6 會詳細說明：

- 多個子行程同時結束時，**SIGCHLD 只會遞送一次**（標準信號不會排隊），所以 handler 裡必須用**迴圈** `waitpid`。
- 父行程阻塞在 `accept` 時收到信號，`accept` 會被中斷（`EINTR`）。`SA_RESTART` 會讓核心自動重新呼叫；程式中 `accept` 的 `EINTR` 重試則是第二層保險。

### 7. 父行程只做 accept + fork，所以不會被卡住

階段 1 的問題是：server 在 `handle_client` 裡阻塞於 `recv` 時，就無法回到 `accept`。現在 `recv` 的阻塞發生在**子行程**裡，父行程 fork 完立刻回到 `accept`。T2 中父行程的狀態是 `S`（阻塞在 accept），兩個子行程也是 `S`（阻塞在 recv），彼此互不影響。

**代價**：每個連線一個行程，建立和切換的成本比 thread 高。但行程之間的記憶體互相隔離，一個子行程當掉不會影響其他連線。這也是本專案要求用 `fork` 而不用 thread 的原因之一（CONTEXT.md B1）。

### 8. 這個階段仍刻意保留的限制

| 限制 | 在哪個階段處理 |
|---|---|
| 殭屍行程累積 | 階段 6：R1（`SIGCHLD` + `waitpid` 迴圈） |
| 閒置的 client 永久佔用一個子行程 | 階段 6：R3（`SO_RCVTIMEO`） |
| 只會 echo | 階段 4：`SYSINFO` / `ECHO` / `QUIT` |
| 還沒有 `--hold`，併發測試要用 `/dev/tcp` 或 `nc` 手動保持連線 | 階段 7：壓測腳本與 `--hold` |

### 9. 自己動手觀察的方法

開三個 WSL 終端機：

```bash
# 終端機 1：啟動 server
./server/server_naive -p 9000

# 終端機 2、3：各開一個互動式連線，輸入文字按 Enter 就會被 echo 回來
nc 127.0.0.1 9000

# 第四個終端機：觀察子行程（把 <pid> 換成 server 的 pid，可從 log 第三欄看到）
ps --ppid <pid> -o pid,ppid,stat,cmd
# 在 nc 中按 Ctrl+C 斷線後再執行一次 → 會看到 Z <defunct>
```
