# 階段 8：封包擷取、報告素材、影片

- **驗收標準**：截圖齊全
- **驗收結果**：
  - ✅ 封包擷取：`scripts/capture.sh` **不需要 sudo**，產生加密、明文、FIN/RST 對照三份 pcap 與文字解碼，連續執行 3 次都穩定
  - ✅ 封包分析：三向交握、密文 payload、明文外洩、FIN 與 RST 斷線都已經確認（見下文）
  - ✅ 文字素材：`docs/results/` 中有編譯 / `ldd` / 兩層 log、12 個 client 壓測、R1–R3 對照的完整輸出
  - ⏳ **截圖與影片需要你自己製作**（Wireshark 在 Windows 上操作、終端機畫面）。清單與步驟見 [`docs/README.md`](../README.md)

---

## 這次建立 / 修改的檔案

| 檔案 | 變更 |
|---|---|
| `scripts/capture.sh` | **新增**：在私有的網路命名空間與 UTS 命名空間中執行 server、client 與 tcpdump，產生三份 pcap 和對應的 `.txt` 解碼；主機名稱以標籤 `REDACTED-HOSTNAME` 取代（第 6 點） |
| `docs/capture/` | `capture.pcap`（k=3）、`capture_plain.pcap`（k=0）、`capture_rst.pcap`（FIN vs RST），以及同名的 `.txt` |
| `docs/results/` | `build.txt`、`stress.txt`、`demo_robust.txt`：各腳本的完整輸出（已去除終端機顏色碼） |
| `docs/README.md` | **新增**：報告素材索引、截圖清單與步驟、2 分鐘影片大綱 |
| `.gitignore` | 改成只追蹤 `docs/capture/*.pcap`（每個只有幾 KB，是報告素材）；其他地方抓的 pcap 不追蹤 |
| `docs/.gitkeep` | 刪除（目錄已有實際檔案） |

```text
./scripts/capture.sh [輸出目錄]        # 預設 docs/capture；系統不允許 unshare 時改用 sudo ./scripts/capture.sh
```

---

## 封包分析

### 1. 三向交握 → 資料 → 斷線（`capture.pcap` 第一條連線：`ECHO hello world`）

```text
 #  方向           Flags  seq / ack                          len   說明
 1  C:58598 → S    [S]    seq 2841532295                      0    SYN：client 選一個隨機初始序號 x
 2  S → C:58598    [S.]   seq 3790021210, ack 2841532296      0    SYN-ACK：server 的初始序號 y，確認 x+1
 3  C → S          [.]    ack 1                               0    ACK：交握完成（之後 tcpdump 改顯示相對序號）
 4  C → S          [P.]   seq 1:18                           17    PSH：送出 "HFKR#khoor#zruog\n"（16 + '\n'）
 5  S → C          [.]    ack 18                              0    server 核心確認收到 17 bytes
 6  S → C          [P.]   seq 1:16                           15    PSH：回覆 "RN#khoor#zruog\n"
 7  C → S          [.]    ack 16                              0
 8  C → S          [F.]   seq 18                              0    FIN：client 印完回覆後 close()
 9  S → C          [F.]   seq 16, ack 19                      0    FIN：server 子行程讀到 EOF → close()
10  C → S          [.]    ack 17                              0    最後的 ACK；client 進入 TIME_WAIT
```

**對照程式碼**：

| 封包 | 由哪一行程式觸發 |
|---|---|
| 1–3 | client 的 `connect()`；server 端由**核心**在 `listen` 之後自動完成，此時 `accept()` 甚至還沒回傳（stage-1.md 第 3 點） |
| 4 | client 的 `send_line()` → `send_all()` → 一次 `send()`，所以內容和 `\n` 在同一個封包裡（stage-2.md 第 4 點） |
| 5、7 | 純 ACK，由核心自動送出，應用程式完全不知道 |
| 6 | server 子行程的 `send_line()` |
| 8 | client 的 `close(fd)` |
| 9 | server 子行程的 `recv_line()` 回傳 0 → `handle_client` 返回 → `close(connfd)` |

**相對序號**：SYN 和 FIN 各佔用 1 個序號。所以資料從 seq 1 開始；client 送了 17 bytes 的資料，加上自己的 FIN，server 最後 ACK 的是 19（1 + 17 + 1）。

**為什麼 server 的 FIN 在 client 之後**：這條連線由 client 主動關閉（**active close**），所以 client 最後會進入 `TIME_WAIT`。如果由 server 主動關閉（例如 QUIT，見第 3 點），`TIME_WAIT` 就會留在 server 端。這正是 server 需要 `SO_REUSEADDR` 的原因（stage-1.md 第 6 點）。

### 2. 密文 vs. 明文（`PSH` 封包的 payload）

| 方向 | `capture.pcap`（k=3） | `capture_plain.pcap`（k=0） |
|---|---|---|
| C→S | `HFKR#khoor#zruog` | `ECHO hello world` |
| S→C | `RN#khoor#zruog` | `OK hello world` |
| C→S | `V\VLQIR` | `SYSINFO` |
| S→C | `RN#krvwqdph@UHGDFWHG0KRVWQDPH>v\|vqdph@Olqx{>uhohdvh@9191447140plfurvriw0vwdqgdug0ZVO5>...` | `OK hostname=REDACTED-HOSTNAME;sysname=Linux;release=6.6.114.1-microsoft-standard-WSL2;...` |
| C→S | `TXLW` | `QUIT` |
| S→C | `RN#e\|h` | `OK bye` |

- **加密版**：線路上看不到任何指令或系統資訊的明文。
- **明文版**：主機名稱、核心版本、記憶體大小、pid 全部直接外洩。任何能在路徑上抓封包的人（同一個 Wi-Fi、路由器、ISP）都看得到。
- **但凱薩加密的結構洩漏也一目了然**：`RN#` 出現在每一則回覆的開頭（= `OK `）；SYSINFO 回覆中 `@`（= `=`）和 `>`（= `;`）規律地交替出現。光看密文，就能推測出這是 `key=value;` 格式，再用「回覆一定以 `OK ` 開頭」反推出 k=3（stage-5.md 第 7 點：已知明文攻擊）。

### 3. FIN vs. RST（`capture_rst.pcap`）

**(a) 正常 QUIT：server 主動以 FIN 關閉**

```text
C → S [P.] len 5     "TXLW\n"（QUIT）
S → C [P.] len 7     "RN#e|h\n"（OK bye）
S → C [F.]           ← server 先送 FIN（dispatch 回傳 quit=1 → close）
C → S [F.]           ← client 收到 EOF 後也關閉
S → C [.]            ← server 是主動關閉方，這次 TIME_WAIT 在 server 端
```

**(b) QUIT 之後還多送一行：server 以 RST 關閉**

```text
C → S [P.] len 5     "TXLW\n"（QUIT）
C → S [P.] len 11    "HFKR#diwhu\n"（ECHO after）← server 不會再讀這一行
S → C [P.] len 7     "RN#e|h\n"（OK bye）
S → C [R.]           ← RST！不是 FIN
```

**原理**：`close()` 時，如果 socket 的**接收緩衝區中還有應用程式沒讀走的資料**，Linux 會送出 **RST** 而不是 FIN（RFC 2525 / RFC 1122 4.2.2.13）。意思是告訴對方：「你送來的資料有一部分被丟掉了，不要以為我都收到了」。這和 stage-6.md R2 的情況剛好相反：R2 是 **client** 帶著未讀資料關閉，所以 client 送 RST，server 再寫入時才收到 SIGPIPE。

### 4. 其他可以從封包看出來的細節

- **MSS 65495**（SYN 的 options，`tcpdump -nn -r` 不加 sed 就看得到）：loopback 介面的 MTU 是 65536，扣掉 IP 與 TCP 標頭就是 65495。實體網路卡通常是 1460（MTU 1500）。
- **每個 `PSH` 都對應一次 `send()`**：本專案每則訊息都很短，一次 `send_all` 就會變成一個封包。但這只是**碰巧**，TCP 本身不保證這種對應關係（stage-1.md 第 7 點）。(b) 中 bash 的 `printf` 分成兩次寫入，就產生了兩個 PSH 封包。
- **window 512**：交握後 tcpdump 顯示的是**縮放前**的值。SYN 中的 `wscale 7` 代表實際視窗 = 512 × 2⁷ = 65536 bytes。

---

## 為什麼這樣寫（含運作原理）

### 1. 為什麼抓封包需要權限

tcpdump 透過 **`AF_PACKET` raw socket** 直接從網路介面複製每一個封包，可以看到**所有行程**的流量（包括其他使用者的）。所以 Linux 要求呼叫者具備 **`CAP_NET_RAW`**（通常只有 root 才有）。一般使用者執行時會得到：

```text
tcpdump: lo: You don't have permission to perform this capture on that device
```

### 2. 不需要 sudo 的方法：使用者命名空間 + 網路命名空間

```bash
unshare -rn ./scripts/capture.sh
#        │└─ -n：新的「網路命名空間」：擁有自己獨立的 lo、路由表、port 空間
#        └── -r：新的「使用者命名空間」：把目前的使用者對應成命名空間內的 root（uid 0）
```

**命名空間（namespace）** 是 Linux 容器（Docker 等）的基礎：讓一群行程看到一份**私有的**系統資源。

- 在新的**使用者命名空間**中，我們是 uid 0，擁有 `CAP_NET_RAW`，但**這個能力只對這個命名空間擁有的資源有效**。
- 新的**網路命名空間**是我們自己建立的，所以可以在**它的** `lo` 上抓封包，但看不到主機真正的 `lo`，也看不到其他程式的流量。
- 因此**完全沒有提升任何權限**：沒有修改系統設定，也沒有使用 sudo。這個方法是否可用，取決於系統是否允許無特權的使用者命名空間（WSL2 預設允許；Ubuntu 實體機若開啟了 AppArmor 的限制，就要改用 `sudo ./scripts/capture.sh`）。

**附帶好處**：命名空間內**只有**這個腳本的流量，port 9000 也一定是空的，不會和你在外面執行的 server 衝突，抓到的封包很乾淨。

**實作時遇到的三個陷阱**：

| 現象 | 原因 | 解法 |
|---|---|---|
| `Couldn't change ownership of savefile` | tcpdump 以 root 執行時，預設會切換成 `tcpdump` 使用者（降低權限），但命名空間中沒有這個使用者的對應 uid | 加上 `-Z root`，不要切換使用者 |
| 讀檔時 `Couldn't change to 'tcpdump' uid` | 用 `-r` 讀檔時也會切換使用者 | 讀檔也加上 `-Z root` |
| **`20 packets received by filter`、`0 packets captured`**，pcap 只有 24 bytes 的檔頭 | 用 `grep "listening on"` 判斷 tcpdump 已就緒並不可靠：錯誤訊息檔中可能有上一輪的舊內容，而且實測發現「listening on」印出時，擷取未必已經完全生效 | 每次先清空錯誤訊息檔，看到 `listening on` 後再等 0.5 秒；並在抓到 0 個封包時**明確報錯**，不默默產生空檔案 |

第三個陷阱是用「控制變因」的方式找到的：只改等待方式，其他條件不變，結果從 0 個封包變成 10 個。

### 3. `tcpdump` 參數

| 參數 | 意義 |
|---|---|
| `-i lo` | 在 loopback 介面上抓。同一台電腦內的連線（127.0.0.1）不會經過實體網卡 |
| `-nn` | 不把 IP 轉成主機名稱、不把 port 轉成服務名稱，顯示原始數字 |
| `-s0` | 抓完整的封包（snapshot length 不限）；不加的話，舊版本只抓前 68 bytes，payload 會被截斷 |
| `-U` | 每抓到一個封包就立刻寫入檔案，不在記憶體中累積 |
| `-w file` | 存成 pcap 二進位檔（Wireshark 可以開啟） |
| `-A` | 讀檔時以 ASCII 顯示 payload |
| `"tcp port 9000"` | BPF 過濾條件：在**核心中**就先過濾掉不相關的封包 |
| `kill -INT` | 讓 tcpdump 正常結束、寫完檔案（直接 `kill -9` 可能讓檔案不完整） |

### 4. 為什麼產生三份 pcap

| 檔案 | 對應 CONTEXT.md B11 的哪一點 |
|---|---|
| `capture.pcap` | 1. 三向交握；2. 資料封包可見為**凱薩密文**；3. 斷線 FIN；4. Wireshark Follow TCP Stream |
| `capture_plain.pcap` | 加分項：「另抓一次不加密的版本，對比明文外洩」 |
| `capture_rst.pcap` | 3. 斷線 `FIN` / `RST`：一般只會看到 FIN，所以刻意製造出 RST 的情境來對照 |

每個指令都使用**獨立的連線**，所以在 Wireshark 中，每條 TCP stream 都是一個完整的「交握 → 一問一答 → 斷線」，截圖時很清楚。

### 5. 為什麼把 pcap 納入版本控制

原本 `.gitignore` 排除了所有 pcap，因為封包檔通常很大。但這三個檔案總共不到 8 KB，而且是報告的直接證據，放進 repo 可以讓別人不用執行就能檢查。所以改成只追蹤 `docs/capture/*.pcap`，在其他地方自己抓的 pcap 仍然會被忽略。

### 6. 敏感資訊的標籤處理：UTS 命名空間

SYSINFO 的回應含有**主機名稱**，而這個 repo 是公開的。開發紀錄等文字檔可以直接把主機名稱換成標籤，但 **pcap 是二進位封包**，直接改其中的 bytes 會造成兩個問題：

- 字串長度一改變，TCP 的序號、長度和 IP 的總長度就全部對不上了。
- 就算長度相同，TCP checksum 也會錯誤，Wireshark 會標示為壞掉的封包。

所以 `capture.sh` 不去修改封包，而是**讓 server 回報的主機名稱本身就是標籤**：

```bash
unshare -rnu ...          # -u：再建立一個私有的 UTS 命名空間
hostname REDACTED-HOSTNAME
```

**UTS 命名空間**（名稱來自 UNIX Time-sharing System）隔離的是 `uname()` 回傳的**主機名稱**與網域名稱。在新的 UTS 命名空間中呼叫 `sethostname()`，只會改變這個命名空間看到的值，系統真正的主機名稱完全不受影響（實測：擷取後在外面執行 `hostname`，仍然是原本的名稱）。server 的 `build_sysinfo()` 呼叫 `uname()`，讀到的就是標籤，所以封包是完整、未經修改的真實封包，只是內容裡沒有真正的主機名稱。

| 細節 | 說明 |
|---|---|
| 標籤為什麼是 `REDACTED-HOSTNAME`，而不是 `<HOSTNAME>` | `hostname` 指令會依 RFC 1123 檢查格式（只允許英數字與 `-`）；直接寫 `/proc/sys/kernel/hostname` 在使用者命名空間中又沒有權限。所以選用符合格式的標籤，文字檔也統一使用同一個標籤 |
| 為什麼連**密文**也要處理 | 加密版 pcap 中的主機名稱是凱薩密文 `UHGDFWHG0KRVWQDPH`（標籤的密文）。如果沒有處理，原本的密文用 k=3 一解就能還原成真正的主機名稱。**凱薩加密不能保護隱私** |
| 以 sudo 執行時 | 也一樣會先 `unshare -nu`，`hostname` 指令永遠只作用在私有的命名空間中，絕不會改到真實系統 |
| 要保留真實主機名稱 | `CAPTURE_HOSTNAME= ./scripts/capture.sh` |

**保留沒有遮蔽的資訊**：核心版本、記憶體大小、行程數、pid、`127.0.0.1` 和 port。這些不能用來識別個人或特定的電腦，而且正是 SYSINFO 要示範的內容。
