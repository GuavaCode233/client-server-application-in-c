# 階段 1：最小 server / client（echo，無 fork、無加密）

- **驗收標準**：連線成功，Client 送一行，Server 回同一行；手動測試成功
- **驗收結果**：✅ 通過（7 項功能測試加 1 項行為觀察，`make` 無任何警告）

---

## 這次建立的檔案

| 檔案 | 變更 |
|---|---|
| `server/server.c` | 改寫為迭代式 echo server：`socket → SO_REUSEADDR → bind → listen → accept 迴圈`，對每個 client「讀一行、回一行」直到對方關閉 |
| `client/client.c` | 改寫為最小 client：把命令列參數串成一行送出，讀回一行後印出並結束 |

命令列介面（CONTEXT.md Part D 第 7 點的子集）：

```text
./server/server_naive [-p port]
./client/client [-h host] [-p port] <text...>
```

### 驗收輸出

```text
--- T1 basic ---
$ ./client/client -p 9100 hello world
hello world

--- T2 中文 / 符號 ---
$ ./client/client -p 9100 "ECHO 你好 !@#"
ECHO 你好 !@#

--- T3 一次送出兩行（測試訊息邊界）---
$ printf 'line-A\nline-B\n' | (透過 /dev/tcp 一次寫入)
line-A
line-B

--- T4 超長輸入（1100 bytes）---
[client] input too long (max 1022 bytes)      exit=1

--- T5 server 不存在 ---
[client] connect 127.0.0.1:9199: Connection refused      exit=1

--- T6 非法 IP ---
[client] invalid IPv4 address: abc      exit=1

--- T7 關掉 server 立刻重啟同一個 port（SO_REUSEADDR）---
after-restart
```

Server 端 log：

```text
[server] (naive, libutil 0.0 (stage 0)) listening on port 9100
[server] 127.0.0.1:54998 connected
[server] 127.0.0.1:54998 recv: hello world
[server] 127.0.0.1:54998 disconnected
...
[server] 127.0.0.1:55012 connected
[server] 127.0.0.1:55012 recv: line-A
[server] 127.0.0.1:55012 recv: line-B      ← 同一次 send 的兩行，被正確切成兩則訊息
[server] 127.0.0.1:55012 disconnected
```

### 行為觀察：迭代式 server 一次只能服務一個 client

```text
A：連上 server，但不送任何資料（佔住 server）
B：./client from-B         → 卡住，2 秒後被 timeout 終止（exit=124）
A：關閉連線
B：./client from-B-again   → 立刻成功
```

Server log：

```text
127.0.0.1:46242 connected        ← A
127.0.0.1:46242 disconnected     ← A 關閉後，server 才回到 accept
127.0.0.1:46256 connected        ← 第一次的 B（此時 B 早就被 timeout 殺掉了）
127.0.0.1:46256 recv: from-B     ← 但資料仍然收得到！
127.0.0.1:46256 disconnected
127.0.0.1:46258 connected        ← 第二次的 B
```

---

## 為什麼這樣寫（含運作原理）

### 1. Socket 是什麼：一切都是檔案描述符

`socket()` 回傳的是一個整數 **fd（file descriptor）**，跟 `open()` 開檔回傳的是同一種東西。它是行程的「fd 表」裡的一個索引，指向核心中的 socket 物件，而 socket 物件內有傳送與接收緩衝區、連線狀態、對方位址等資料。所以 socket 也能用 `close()` 關閉，fd 也會在 `fork()` 時被複製（階段 3 的重點）。

### 2. Server 流程：每個系統呼叫在核心裡做了什麼

| 呼叫 | 核心做的事 |
|---|---|
| `socket(AF_INET, SOCK_STREAM, 0)` | 建立一個 IPv4 的 TCP socket 物件，此時還沒有位址 |
| `setsockopt(SO_REUSEADDR)` | 允許 bind 到仍有 `TIME_WAIT` 舊連線的 port（見第 6 點） |
| `bind(addr)` | 把 socket 綁定到「本機 IP : port」；`INADDR_ANY` 代表所有網路介面 |
| `listen(fd, 16)` | 把 socket 轉成**被動**模式。從這時起，**核心會自己回應 SYN、完成三向交握**，並把完成的連線放進佇列 |
| `accept(fd)` | 從佇列取出一條已完成的連線，回傳一個**新的 fd**（connfd）；佇列空的就阻塞等待 |

這裡要注意，**有兩種 fd**：

- `listenfd`：只負責「接新連線」，永遠不拿來傳資料。
- `connfd`：每條連線一個，專門跟那個 client 對話。

### 3. 三向交握在 `listen` 時就完成，不是在 `accept` 時

上面的「行為觀察」證明了這一點：

1. server 正在服務 A，沒有呼叫 `accept`。
2. B 呼叫 `connect`，**核心**替 server 回了 SYN-ACK，三向交握完成，B 的 `connect()` 成功回傳。
3. B 把 `from-B\n` 寫出去，資料放進 server 端那條連線的**接收緩衝區**，此時還沒有人讀。
4. B 一直等不到回覆，2 秒後被 timeout 殺掉，B 的核心送出 FIN。
5. A 斷線後，server 回到 `accept`，才拿到 B 這條「已建立、有資料、而且已經收到 FIN」的連線，於是還是讀得到 `from-B`，接著讀到 0（FIN），印出 disconnected。

結論是：`listen` 的 backlog 佇列裝的是**已完成交握**的連線，`accept` 只是把它交給應用程式。所以 B 的 `connect` 成功，不代表 server 真的開始處理它了。

### 4. Client 流程：不需要 bind

`connect()` 會主動送出 SYN 發起三向交握，等收到 SYN-ACK 並回 ACK 後才回傳。Client 沒有呼叫 `bind`，核心會自動配一個**暫時埠（ephemeral port）**，就是 log 裡的 `54998`、`55006` 這些號碼，每次都不同。

### 5. 網路位元組序：`htons` / `htonl` / `inet_pton`

不同 CPU 存多位元組整數的順序不同。x86 是 little-endian（低位元組在前），而網路協定規定一律用 **big-endian**（network byte order）。

- `htons(9000)`：host → network，short（16 位元的 port）
- `htonl(INADDR_ANY)`：host → network，long（32 位元的 IP）
- `ntohs(cli.sin_port)`：反方向，印 log 時把 port 轉回來
- `inet_pton("127.0.0.1")`：文字 IP → 32 位元的網路位元組序位址；`inet_ntop` 則反過來

如果忘了 `htons`，在 x86 上 9000（`0x2328`）會被當成 `0x2823`（10275），server 會聽錯 port。

### 6. `SO_REUSEADDR`：為什麼重啟會 bind 失敗

TCP 連線關閉後，**主動關閉的那一方**會進入 `TIME_WAIT` 狀態，停留 2×MSL（Linux 約 60 秒）。目的是確保最後一個 ACK 送達，並讓網路上殘留的舊封包消失，不會被誤認成新連線的資料。

Server 被 kill 時，它那端的連線就會進入 `TIME_WAIT`。這段時間內，沒設 `SO_REUSEADDR` 的新 server 呼叫 `bind` 會得到 `Address already in use`。T7 測試就是在 kill 之後立刻重啟，確認可以 bind。

> 這原本列在 CONTEXT.md B9 的 R5（選配強健性），但 B8 的 server 流程第一步就要求設定，而且開發過程中會頻繁重啟 server，所以在 naive 和 robust 兩個版本都先設定。

### 7. TCP 是位元組串流：用 `\n` 切訊息

TCP 只保證「送出的位元組會**按順序、不遺失**地到達」，**不保證**「一次 `send` 對應一次 `recv`」：

- 送兩行，可能一次 `recv` 全部收到（T3 就是這種情況）。
- 送一行，也可能分好幾次才收完（網路壅塞、封包分段等）。

所以應用層必須自己定義**訊息邊界**。本專案規定「一行以 `\n` 結尾就是一則訊息」。`read_line_simple()` 一次只讀 1 個 byte，讀到 `\n` 就停，這樣：

- 不會多讀到下一行的內容（那部分繼續留在核心緩衝區，下次再讀）。
- 不論 TCP 怎麼切封包，結果都一樣。

T3 的 log 中 `line-A`、`line-B` 被分成兩次 `recv:` 印出，證明這個切法是對的。

**代價**是每個 byte 都要一次系統呼叫（從使用者空間切到核心再切回來），效率很差。階段 2 的 `recv_line` 會改成「一次 recv 一大塊放進緩衝區，再從緩衝區切行」。

### 8. `recv` 的三種回傳值

| 回傳 | 意義 | 程式的處理 |
|---|---|---|
| `> 0` | 收到這麼多 bytes | 放進緩衝區 |
| `0` | **對方正常關閉**（收到 FIN），不是錯誤 | 結束這個 client |
| `-1` | 錯誤，看 `errno` | `EINTR`（被信號中斷）就重試，其他錯誤就記 log 並結束 |

如果行讀到一半就收到 0，代表對方沒送完就斷線，回傳 -1，視為不完整。

### 9. 緩衝區安全

- `read_line_simple` 最多寫入 `maxlen - 1` bytes，保留 1 byte 給 `'\0'`；塞滿還沒看到 `\n` 就判定為超長。
- client 串接參數時用 `snprintf` 並帶入剩餘長度，檢查回傳值是否被截斷（T4）。
- client 拒絕內含 `\n` 的輸入，因為那會被 server 當成兩則訊息，破壞協定。

這些都對應 CONTEXT.md Part D 第 4 點：緩衝區操作一律帶長度，字串確保以 `\0` 結尾。

### 10. 階段 1 刻意留下的簡化（之後的階段會補上）

| 簡化 | 問題 | 在哪個階段處理 |
|---|---|---|
| 一次讀 1 byte | 效率差 | 階段 2：`recv_line` |
| `send` 一次就假設送完 | 可能部分寫入（short write） | 階段 2：`send_all` |
| 用 `fprintf(stderr)` 印錯誤 | 沒有時間、pid、等級 | 階段 2：`LOG_E` / `LOG_I` / `LOG_D` |
| 迭代式，一次只服務一個 client | 一個閒置的 client 就讓其他人全部卡住（見行為觀察） | 階段 3：`fork()` |
| 閒置的 client 會永久佔住 server | `recv` 永遠阻塞 | 階段 6：R3 `SO_RCVTIMEO` |
| `read_line_simple` 在 server 和 client 各寫一份 | 重複程式碼 | 階段 2：移進 `libutil.so` |

### 11. 為什麼 client 暫時不連結 libutil.so

Client 這個階段沒有呼叫 `libutil.so` 的任何函式。因為 Ubuntu 預設的 `--as-needed`（見 stage-0.md 第 3 點），`ldd client/client` 暫時不會列出 `libutil.so`。Server 啟動訊息有呼叫 `util_version()`，所以 server 仍然連結著它。階段 2 把 `recv_line` / `send_all` 移進 `.so` 後，client 會真正用到 `.so` 的函式，就會再次連結上。
