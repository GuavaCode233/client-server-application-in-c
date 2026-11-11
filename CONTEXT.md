# 期中專案 Context 文件：網路系統程式開發與安全

> 用途：(1) 作為我（學生）的學習地圖；(2) 作為 Claude Code 開發時的參考規格。
> 開發原則：**先求最小可跑版本，再逐步加功能；每一步都要讓我看得懂。**

---

## Part A：我需要學的概念（簡述，詳細自行研究）

建議學習順序由上而下，每項附搜尋關鍵字。

| # | 概念 | 一句話說明 | 關鍵字 |
|---|---|---|---|
| 1 | Client–Server 模型 | Server 被動等待連線，Client 主動發起連線 | client server model |
| 2 | Socket | 程式對網路通訊的抽象介面，本質上是一個檔案描述符（fd） | `socket()` man page |
| 3 | TCP vs UDP | 本專案用 TCP：可靠、有連線、保證順序 | TCP three-way handshake |
| 4 | Server 流程 | `socket → bind → listen → accept → recv/send → close` | bind listen accept |
| 5 | Client 流程 | `socket → connect → send/recv → close` | connect |
| 6 | IP / Port / 位元組序 | `htons`、`htonl`、`inet_pton`、`sockaddr_in` | network byte order |
| 7 | TCP 是位元組串流 | 一次 `recv` 不保證收到完整訊息，需自行定義訊息邊界（本專案用 `\n`） | TCP message framing |
| 8 | 部分讀寫與 `EINTR` | `send` 可能只送出一部分；系統呼叫可被信號中斷 | short write, EINTR |
| 9 | `fork()` | 複製行程；回傳 0 為子行程，>0 為父行程 | fork parent child |
| 10 | fd 在 fork 後的處理 | 父關 `connfd`、子關 `listenfd`，否則資源洩漏、連線無法正確關閉 | close fd after fork |
| 11 | 殭屍行程（zombie） | 子行程結束後父行程沒 `wait`，會留下 `<defunct>` | zombie process |
| 12 | 信號（signal） | `SIGCHLD`、`SIGPIPE`、`SIGINT`；用 `sigaction` 設定 | sigaction SA_RESTART |
| 13 | `waitpid(-1, NULL, WNOHANG)` | 在 SIGCHLD handler 內迴圈回收所有已結束的子行程 | waitpid WNOHANG |
| 14 | `SIGPIPE` | 對已關閉的連線寫入會收到此信號，預設終止行程 | SIGPIPE MSG_NOSIGNAL |
| 15 | Socket 選項 | `SO_REUSEADDR`（重啟不 bind 失敗）、`SO_RCVTIMEO`（接收逾時） | setsockopt |
| 16 | 動態共享函式庫 `.so` | 編譯成 `-fPIC -shared`；執行時由動態連結器載入 | shared library fPIC rpath |
| 17 | `LD_LIBRARY_PATH` / `rpath` | 讓執行檔找得到 `.so` | `-Wl,-rpath` |
| 18 | 條件編譯 | `#ifdef`、`-D` 巨集；用來做編譯期 log 開關 | conditional compilation |
| 19 | 可變參數巨集 | `__VA_ARGS__`、`__FILE__`、`__LINE__` 用於 log 巨集 | variadic macro |
| 20 | Makefile | 目標、相依、變數、`-I -L -l` | makefile tutorial |
| 21 | `uname()` / `sysinfo()` | 取得 OS 與系統資訊（Linux） | uname sysinfo |
| 22 | tcpdump / Wireshark | 抓封包並分析握手、資料、斷線 | tcpdump loopback |
| 23 | 凱薩加密 | 字元位移 k 位；僅示範概念，不具實際安全性 | Caesar cipher |
| 24 | 為何凱薩不安全 | 金鑰空間極小、可頻率分析；實務用 TLS | frequency analysis TLS |

---

## Part B：專案規格（給 Claude Code 的開發參考）

### B1. 環境假設
- 作業系統：Linux（或 WSL）。`sysinfo()` 為 Linux 專用；若需 macOS 支援，另改用 `sysctl`。
- 語言：C（gnu11），編譯器 gcc，建置工具 Makefile。
- 編譯參數：`-Wall -Wextra -g`。
- **禁止使用 thread（pthread）**，必須用 `fork()`。
- 先做「最小範圍」，不要自行擴充功能。

### B2. 專案目錄結構

```
midterm/
├── Makefile
├── CONTEXT.md
├── include/
│   └── util.h          # 共用函式庫公開介面
├── lib/
│   └── util.c          # → 編譯成 libutil.so
├── server/
│   └── server.c
├── client/
│   └── client.c
├── scripts/
│   ├── stress.sh       # 併發 10+ client 證明
│   └── demo_robust.sh  # 強健性對照示範
└── docs/               # 截圖、pcap、報告素材
    └── dev-log/        # 每階段開發紀錄（stage-0.md、stage-1.md …）
```

### B3. 功能範圍（最小）

**指令（Client → Server）**

| 指令 | 說明 | 回應 |
|---|---|---|
| `SYSINFO` | 請求系統資訊 | `OK hostname=..;sysname=..;release=..;machine=..;uptime=..;totalram=..;freeram=..;procs=..;server_pid=..;child_pid=..` |
| `ECHO <text>` | 回傳文字（用來測試加解密） | `OK <text>` |
| `QUIT` | 結束連線 | `OK bye`，之後 server 關閉連線 |
| 其他 | 未知指令 | `ERR unknown command` |

### B4. 通訊協定
- **文字行協定**：每則訊息為一行，以 `\n` 結尾。這是唯一的訊息邊界。
- 回應固定為**單行**（避免多行框架的複雜度）。
- 單行最大長度：`MAX_LINE = 1024`，超過視為錯誤。
- 預設連接埠：`9000`，可由命令列參數指定。

### B5. 凱薩加密規格（加密範圍已定案）
- **加密對象**：每一行訊息的**內容本體**（不含結尾 `\n`）。雙向皆加密：
  `Client 送出 加密後的指令 → Server 解密後執行 → Server 回傳 加密後的回應 → Client 解密後顯示`
- **演算法**：對可印出 ASCII（32–126，共 95 個字元）做位移：
  - 加密：`c' = 32 + ((c - 32 + k) mod 95)`
  - 解密：`c' = 32 + ((c - 32 - k + 95*N) mod 95)`（注意負數取模，需寫成安全形式）
- 範圍外字元（如 `\n`、控制字元）**不處理**。因為密文必落在 32–126，不會出現 `\n`，不影響分行。
- 金鑰 `k` 預設為 `3`，雙方事先約定（可用命令列 `-k` 覆寫）。
- 實作位置：`caesar_encrypt()` / `caesar_decrypt()` 放在 `libutil.so`。
- 報告需說明：此為示範，可被暴力破解與頻率分析；實務應使用 TLS。

### B6. 共用函式庫 `libutil.so` 介面（`include/util.h`）

```c
/* --- Caesar --- */
void caesar_encrypt(char *s, int k);          // in-place
void caesar_decrypt(char *s, int k);

/* --- 網路 I/O --- */
ssize_t send_all(int fd, const void *buf, size_t len);  // 處理部分寫入、EINTR
                                                        // （不加 MSG_NOSIGNAL，讓 naive 版能重現 R2；robust 版由 server 忽略 SIGPIPE）
ssize_t recv_line(int fd, char *buf, size_t maxlen);    // 讀一行（含 '\n'）；回傳長度，0=對方關閉，-1=錯誤（EMSGSIZE=超長）
int     send_line(int fd, const char *line);            // 自動補 '\n'

/* --- Log --- */
enum { LOG_ERROR = 0, LOG_INFO = 1, LOG_DEBUG = 2 };
void log_set_level(int level);                // 執行期等級
void log_init_from_env(void);                 // 讀環境變數 LOG_LEVEL
void log_write(int level, const char *file, int line, const char *fmt, ...);

#define LOG_E(...) log_write(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_I(...) log_write(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#ifdef ENABLE_DEBUG                            /* 編譯期開關 */
#define LOG_D(...) log_write(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#else
#define LOG_D(...) ((void)0)
#endif
```

### B7. 雙層 Debug Log
1. **編譯期**：`make DEBUG=1` 會加上 `-DENABLE_DEBUG`；未加時 `LOG_D` 完全不進入執行檔（可用 `strings` 驗證）。
2. **執行期**：以 `-v` 參數或環境變數 `LOG_LEVEL=0|1|2` 控制輸出等級。
   - 注意：即使編譯期開啟了 DEBUG，執行期等級低於 2 時仍不輸出。
- Log 格式：`[時間][等級][pid][檔案:行號] 訊息`（pid 對多行程除錯很重要）。

### B8. Server 行為規格
1. `socket → setsockopt(SO_REUSEADDR) → bind → listen(backlog=16)`。
2. 迴圈 `accept`；成功後 `fork()`：
   - **子行程**：關閉 `listenfd`，進入 `handle_client(connfd)`，結束時 `close` 並 `exit(0)`。
   - **父行程**：關閉 `connfd`，回到 `accept`。
3. 每個 client 的連線、斷線、指令都要 log（含 client IP:port 與子行程 pid）。
4. 必須可同時服務 ≥ 10 個 client。

### B9. 強健性機制（以編譯期巨集 `ROBUST` 切換）
**同一份原始碼編出兩個版本**，方便做「有 / 無」對照：
- `server_naive`：不定義 `ROBUST`
- `server_robust`：定義 `-DROBUST`

| # | 異常情況 | 無機制（naive）的後果 | 有機制（robust）的做法 |
|---|---|---|---|
| R1 | 殭屍行程 | client 斷線後 `ps` 出現大量 `<defunct>` | `sigaction(SIGCHLD)` handler 內 `while (waitpid(-1, NULL, WNOHANG) > 0);`，並設 `SA_RESTART` |
| R2 | 對已斷線連線寫入（SIGPIPE） | 子行程被信號直接終止，來不及記 log 與清理 | `signal(SIGPIPE, SIG_IGN)` 或 `MSG_NOSIGNAL`，檢查 `send` 回傳值並優雅結束 |
| R3 | Client 閒置不送資料 | 子行程永久阻塞在 `recv`，資源被佔用 | `setsockopt(SO_RCVTIMEO)`，逾時後 log 並關閉連線 |
| R4（選配） | 超長輸入 | 固定緩衝區溢位風險 | `recv_line` 限制 `MAX_LINE`，超過回傳 `ERR line too long` 並斷線 |
| R5（選配） | Server 重啟 bind 失敗 | `Address already in use` | `SO_REUSEADDR` |
| R6（選配） | `accept` / `recv` 被信號中斷 | 誤判為錯誤而結束 | 遇 `EINTR` 重試 |

> **最少交付 3 組對照：R1、R2、R3。** 每組都要有「指令 + 預期現象 + 截圖」。

### B10. 併發證明（≥ 10 clients）
- `scripts/stress.sh`：同時啟動 12 個 client，每個連線後保持約 15 秒（例如 client 加 `--hold N` 選項）。
- 證明素材：
  - server log：12 筆不同子行程 pid 的連線紀錄
  - `ps --ppid <server_pid> -o pid,stat,cmd`：看到 12 個子行程
  - `ss -tn state established '( sport = :9000 )'`：12 條連線

### B11. 封包擷取與分析
- 同機測試用 loopback：`sudo tcpdump -i lo -nn -A -s0 port 9000 -w docs/capture.pcap`（macOS 為 `lo0`）。
- 分析重點：
  1. 三向交握 `SYN → SYN-ACK → ACK`
  2. 資料封包 `PSH,ACK` 的內容：**可見為凱薩密文而非明文**（可與未加密對照）
  3. 斷線 `FIN` / `RST`
  4. 用 Wireshark「Follow TCP Stream」截圖
- 加分：另抓一次「不加密」版本，對比明文外洩。

### B12. Makefile 需求
- 目標：`all`、`lib`、`server_naive`、`server_robust`、`client`、`clean`。
- 變數：`DEBUG=1` 啟用編譯期 log、`PORT` 可選。
- 函式庫：`gcc -fPIC -shared -o lib/libutil.so lib/util.c -Iinclude`。
- 執行檔連結：`-Llib -lutil -Wl,-rpath,'$$ORIGIN/../lib'`，不需手動設 `LD_LIBRARY_PATH`。
- 驗證：`ldd server/server_robust` 要能看到 `libutil.so`。

---

## Part C：開發階段與驗收標準

| 階段 | 內容 | 驗收方式 |
|---|---|---|
| 0 | 目錄 + Makefile + 空的 `libutil.so` 可編譯 | `make` 成功、`ldd` 看到 `.so` |
| 1 | 最小 server/client：連線、Client 送一行、Server 回同一行（無 fork、無加密） | 手動測試成功 |
| 2 | `.so` 加入 `send_all` / `recv_line` + log 兩層控制 | `make` 與 `make DEBUG=1` 輸出不同；`LOG_LEVEL` 生效 |
| 3 | 加入 `fork()` 處理多 client（**此時先不做 SIGCHLD**） | 兩個 client 同時連線；觀察 zombie |
| 4 | 實作 `SYSINFO` / `ECHO` / `QUIT` | 回傳內容正確 |
| 5 | 加入凱薩加解密（雙向） | tcpdump 看到密文；兩端結果正確 |
| 6 | 強健性 R1、R2、R3（`ROBUST` 切換） | 三組對照可重現 |
| 7 | 壓測腳本、10+ 併發證明 | `ps` / `ss` / log 皆可驗證 |
| 8 | 封包擷取、報告素材、影片 | 截圖齊全 |

---

## Part D：給 Claude Code 的工作規則

1. **教學導向**：每階段只做一件事，完成後簡述「這段程式為何這樣寫」，再進入下一階段。
2. 程式碼註解使用**繁體中文**，重點說明「為什麼」而非「做什麼」，特別是 fork 後 fd 的處理、信號、緩衝區與 `\n` 邊界。
3. 所有系統呼叫都要檢查回傳值並以 `LOG_E` 記錄 `strerror(errno)`。
4. 緩衝區操作一律帶長度，字串確保以 `\0` 結尾；禁止 `gets`、不受限的 `strcpy`/`sprintf`。
5. 不使用 thread，不引入第三方函式庫，不擴充超出 B3 的功能。
6. 不要一次產生全部程式；每階段完成都要實際編譯並以指令測試，回報結果。
7. 命令列介面：
   - Server：`./server [-p port] [-k shift] [-v level] [-t timeout_sec]`
   - Client：`./client [-h host] [-p port] [-k shift] [-v level] [--hold sec] <command...>`
8. 完成標準（Definition of Done）：B3–B12 全部滿足，並能在**另一台乾淨的電腦**上用 `make` 一次編譯、直接執行 demo。
9. **Git 自動提交與推送**：上一次的專案修改，在我**下次提出開發請求時**自動 commit 並 push 到 GitHub（`origin/main`）。
   - 若我在該次請求中**提及任何問題**（錯誤、不符預期、要修改），則**不 commit、不 push**，先處理問題。
   - commit 前須先實際編譯驗證通過；commit 訊息以英文撰寫，標明階段。
10. **每階段開發紀錄**：每完成一個階段，在 `docs/dev-log/stage-<N>.md` 建立一份文件，包含：
    - **這次建立的檔案**：檔案清單、用途、驗收指令與實際輸出。
    - **為什麼這樣寫（含運作原理）**：設計理由，以及背後的系統 / 網路運作原理（例如 fork 後的 fd、信號遞送、動態連結器載入流程等）。
    - 這些紀錄同時是最終報告的素材。
11. **最終報告形式**：書面報告 **PDF**；**英文為主、中文為輔**（正文以英文撰寫，關鍵術語或段落可附中文說明）。

---

## Part E：常見坑（開發前先讀）

- 忘記在父行程 `close(connfd)` → 連線無法完整關閉、fd 洩漏。
- `recv` 回傳 `0` 代表對方正常關閉，不是錯誤。
- 以為一次 `send` 對應一次 `recv`；TCP 不保留訊息邊界。
- SIGCHLD handler 只呼叫一次 `waitpid`，多個子行程同時結束會漏收。
- 信號中斷導致 `accept`/`recv` 回傳 `-1` 且 `errno == EINTR`，需判斷後重試。
- 凱薩解密的負數取模：C 的 `%` 對負數可能得負值，需先加上 95。
- `.so` 找不到：執行時錯誤 `cannot open shared object file`，檢查 `rpath` 或 `LD_LIBRARY_PATH`。
- tcpdump 需 root 權限；現場 Demo 前確認自己電腦已安裝並可執行。
- 加密後的字串仍須是單行（不含 `\n`），這是本設計能運作的前提。

---

## Part F：交付物檢查清單

> 報告形式：書面報告 **PDF**，英文為主、中文為輔（見 Part D 第 11 點）。素材來源：`docs/dev-log/`。

- [ ] 書面報告 PDF（英文為主、中文為輔）
- [ ] 每階段開發紀錄 `docs/dev-log/stage-0.md` ～ `stage-8.md`
- [ ] 系統設計說明（架構圖、協定格式、流程圖）
- [ ] 關鍵程式碼說明（fork、`.so`、log、強健性、凱薩）
- [ ] 截圖：10+ 併發證明
- [ ] 截圖：tcpdump / Wireshark 分析（握手、密文、斷線）
- [ ] 截圖：R1、R2、R3 的「有 / 無」對照（共 ≥ 3 組）
- [ ] 完整原始碼 + Makefile
- [ ] 2 分鐘說明影片（加分）
- [ ] Demo 前在自己電腦完整演練一次
