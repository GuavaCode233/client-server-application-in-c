# 階段 7：壓測腳本與 10+ 併發證明

- **驗收標準**：`ps` / `ss` / log 皆可驗證
- **驗收結果**：✅ 通過。12 個 client 同時連線並各保持 15 秒：12 個子行程、12 條 ESTABLISHED 連線、log 中有 12 個不同的子行程 pid、回覆中有 12 個不同的 `child_pid`，總耗時 15.0 秒（逐一服務需要 180 秒）。另外加測了 naive 版 12 個、robust 版 50 個 client，也全部通過

---

## 這次建立 / 修改的檔案

| 檔案 | 變更 |
|---|---|
| `client/client.c` | 新增 `--hold sec`（0–3600）：收到回覆後保持連線 N 秒才關閉；改用 `getopt_long`，選項字串加上 `+`，讓指令後面的參數（例如 `ECHO -5`）不會被誤認成選項；hold 之前先 `fflush(stdout)` |
| `scripts/stress.sh` | **新增**：同時啟動 N 個 client，收集 `ps` / `ss` / server log / 回覆中的 `child_pid` 四種證明，並自動判定是否通過 |

命令列：

```text
./client/client [-h host] [-p port] [-k shift] [-v level] [--hold sec] <command...>
./scripts/stress.sh [N=12] [HOLD=15]          # 環境變數：PORT=9000 VARIANT=robust|naive CMD=SYSINFO
./scripts/stress.sh | tee docs/stress.txt     # 存成報告素材
```

### 驗收輸出（`./scripts/stress.sh 12 15`，robust 版）

```text
server_robust pid=495 port=9000；同時啟動 12 個 client，每個保持 15 秒，指令：SYSINFO

$ ps --ppid 495 -o pid,stat,cmd
    PID STAT CMD
    512 S+   ./server/server_robust -p 9000 -t 45
    513 S+   ./server/server_robust -p 9000 -t 45
    ...（共 12 行，pid 512–523）
    523 S+   ./server/server_robust -p 9000 -t 45

$ ss -tn state established '( sport = :9000 )'
Recv-Q Send-Q Local Address:Port Peer Address:Port
0      0          127.0.0.1:9000    127.0.0.1:38100
0      0          127.0.0.1:9000    127.0.0.1:38152
    ...（共 12 條，每條的 client 端 port 都不同）

$ server log（connected 紀錄）
[INFO ][512][server/server.c:438] 127.0.0.1:38076 connected (child pid=512, connfd=4)
[INFO ][513][server/server.c:438] 127.0.0.1:38078 connected (child pid=513, connfd=4)
    ...（共 12 筆）
[INFO ][523][server/server.c:438] 127.0.0.1:38174 connected (child pid=523, connfd=4)

$ 各 client 收到的回覆（節錄 child_pid）
  client 1  server_pid=495;child_pid=516
  client 2  server_pid=495;child_pid=515
    ...（12 個 client 的 server_pid 都相同，child_pid 各不相同）

結果
  ✔ 同時存在的子行程：12
  ✔ ESTABLISHED 連線：12
  ✔ log 中不同的子行程 pid：12
  ✔ 回覆中不同的 child_pid：12
  ✔ client 全部成功結束（exit 0）
  ✔ log 中的 disconnected：12
  總耗時 15.0 秒（若是逐一服務會需要 12 × 15 = 180 秒）
  ✔ 結束後剩餘子行程：0（R1 已回收）
```

### 加測

| 測試 | 結果 |
|---|---|
| `VARIANT=naive ./scripts/stress.sh 12 3` | 前 6 項全部通過，耗時 3.0 秒；結束後留下 **12 個殭屍**（R1 對照） |
| `./scripts/stress.sh 50 3`（超過 backlog 16） | 50 個子行程、50 條連線，全部通過，耗時 3.0 秒，結束後剩 0 個子行程 |
| `./client/client ECHO -5 dash-arg-kept` | `OK -5 dash-arg-kept`（`-5` 沒有被當成選項） |
| `--hold 2` | 實際經過 2 秒；`--hold abc`、`--hold 4000` 都被拒絕 |

---

## 為什麼這樣寫（含運作原理）

### 1. 為什麼需要 `--hold`

一般的 client 送出指令、收到回覆後就馬上斷線，整個過程只要幾毫秒。就算同時啟動 12 個，也很可能前一個已經結束、後一個才剛連上，**`ps` 和 `ss` 根本來不及看到 12 個同時存在**。

`--hold N` 讓 client 收到回覆後**保持連線** N 秒，server 端對應的子行程也會一直阻塞在 `recv` 等下一行指令。這樣就有一段 N 秒的時間窗，可以同時觀察到所有的連線和子行程。

**為什麼在收到回覆「之後」才 hold**：

- 回覆中包含 `child_pid`，可以立刻得到「每個 client 都由不同子行程服務」的第四種證明。
- 先 hold 再送指令也可以，但這樣就要等 N 秒後才拿得到回覆，腳本會比較複雜。

### 2. `fflush(stdout)`：stdio 緩衝的三種模式

| 輸出目標 | 緩衝模式 | 什麼時候真正寫出 |
|---|---|---|
| 終端機 | 行緩衝（line buffered） | 遇到 `\n` 時 |
| 檔案、管線 | **全緩衝**（fully buffered） | 緩衝區滿了（通常 4 KB）、呼叫 `fflush`、或程式結束時 |
| stderr | 無緩衝 | 立刻 |

壓測腳本把 client 的 stdout 導向檔案（`> client_$i.out`），所以是**全緩衝**。如果不 `fflush`，`printf` 的回覆會一直留在記憶體裡，要等 15 秒後程式結束才會寫進檔案，腳本在 hold 期間讀到的就是空檔案。

這和 stage-3.md 第 3 點「fork 後 stdio 緩衝區被複製」是同一個機制：**stdio 的緩衝區在使用者空間，核心看不到**。

### 3. `sleep()` 的迴圈

```c
unsigned left = hold;
while (left > 0)
    left = sleep(left);
```

`sleep()` 被信號打斷時會**提早回傳剩餘的秒數**。用迴圈把剩下的時間睡完，才能保證真的 hold 滿 N 秒。這是處理「可被中斷的系統呼叫」的同一種模式（R6 的 `EINTR` 重試）。

### 4. `getopt_long` 與選項字串開頭的 `+`

- **`getopt_long`**：`getopt` 只支援 `-h` 這種單一字母的短選項，CONTEXT.md Part D 規定的 `--hold` 是長選項，所以要改用 `getopt_long`，並在 `struct option` 陣列中宣告 `{"hold", required_argument, NULL, 'H'}`。
- **開頭的 `+`**：GNU `getopt` 預設會**重新排列** `argv`，把所有看起來像選項的參數（以 `-` 開頭的）都挑出來處理，不管它出現在哪裡。因此 `./client ECHO -5` 中的 `-5` 會被當成未知的選項，程式就失敗了。選項字串以 `+` 開頭時，會改成 POSIX 的行為：**遇到第一個非選項參數就停止解析**，之後的參數全部原樣當成指令內容。

> 這個問題其實從階段 1 就存在了，只是之前的測試沒有剛好送出以 `-` 開頭的文字。

### 5. 壓測腳本的設計

```text
啟動 server（-t 設成 HOLD+30，避免 R3 在壓測途中誤踢）
   │
   ├─ for i in 1..N:  ./client --hold HOLD SYSINFO > client_i.out &    ← 全部丟到背景，幾乎同時發出
   │
   ├─ sleep 1（等連線建立完、server 完成 fork）
   │
   ├─ 收集證明（此時 N 條連線都還在）：ps / ss / server log / 各 client 的回覆
   │
   ├─ wait 所有 client（大約 HOLD 秒）
   │
   └─ 自動判定：子行程數、連線數、不同的 pid 數、client exit code、disconnected 數、殘留子行程
```

| 設計 | 原因 |
|---|---|
| 四種**互相獨立**的證明 | `ps` 看行程、`ss` 看核心中的 socket 狀態、server log 是 server 自己的紀錄、`child_pid` 是 client 收到的回覆。四個來源彼此獨立，結果一致才有說服力 |
| 自動判定 ✔ / ✘，並以 exit code 回報 | 不必用眼睛數 12 行；也可以拿來當作自動化測試 |
| 記錄總耗時 | 12 × 15 秒的工作在 15.0 秒內完成，是「真的同時處理」最直接的證據 |
| `-t $((HOLD + 30))` | robust 版預設 30 秒逾時。如果使用者把 HOLD 設成 40，沒有這個調整的話，連線會在途中被 R3 踢掉，測試就會失敗 |
| `trap cleanup EXIT` | 中途按 Ctrl+C 也會把背景的 client 和 server 全部清掉 |
| 只用 `bash`、`ps`、`ss`、`awk` | 不需要 root；不用 `bc`（乾淨的 Ubuntu 不一定有安裝） |

### 6. 為什麼 50 個 client 也沒問題：backlog 的真正意義

`listen(fd, 16)` 中的 16，**不是**「最多同時服務 16 個 client」，而是「**已完成三向交握、但還沒被 `accept` 取走**」的連線佇列長度（stage-1.md 第 3 點）。

```text
connect ──▶ [核心完成交握] ──▶ backlog 佇列（≤ 16）──accept──▶ fork ──▶ 子行程服務
                                                     ▲
                                        父行程只做這件事，速度很快
```

父行程 `accept` 一條連線後立刻 `fork`，然後就回去 `accept` 下一條，每條連線只需要零點幾毫秒，所以佇列幾乎不會累積。真正的同時服務數量，受限的是**行程數上限**（`ulimit -u`）和 **fd 數上限**（每個子行程只用 1 個 fd，父行程只用 1 個 listenfd，所以不是瓶頸）。

如果佇列真的滿了，Linux 會**直接丟掉新的 SYN**（不回應），client 的核心會自動重送 SYN。所以 client 只會感覺「連線比較慢」，而不是馬上失敗。

### 7. 從證明素材讀出的細節（寫報告時可以用）

- **每條連線的 client 端 port 都不同**（38076、38078……）：這些是核心自動分配的**暫時埠**（stage-1.md 第 4 點）。一條 TCP 連線由「來源 IP、來源 port、目的 IP、目的 port」四個值唯一識別，所以同一台 client 可以對同一個 server port 開很多條連線。
- **每個子行程的 `connfd` 都是 4**：fd 是每個行程各自一張表的索引（stage-3.md）。
- **子行程的 pid 是連續的**（512–523）：fork 是依序發生的。
- **`ps` 的 CMD 欄位和父行程完全相同**：fork 不會換程式，子行程只是父行程的複本（沒有 `exec`）。
- **`Recv-Q` / `Send-Q` 都是 0**：所有資料都已經被讀走或送出，連線處於閒置狀態（阻塞在 `recv`）。

### 8. 這個階段刻意沒有做的部分

| 項目 | 原因 |
|---|---|
| 把壓測結果自動存進 `docs/` | 由使用者決定什麼時候存：`./scripts/stress.sh \| tee docs/stress.txt` |
| 封包擷取、截圖、報告 | 階段 8 |
