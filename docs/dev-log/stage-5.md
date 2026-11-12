# 階段 5：凱薩加解密（雙向）

- **驗收標準**：tcpdump 看到密文；兩端結果正確
- **驗收結果**：
  - ✅ 兩端結果正確（單元測試涵蓋 401 個金鑰，端對端測試包括 SYSINFO / ECHO / QUIT / 未知指令）
  - ✅ 線路上是密文：用 `nc` 假扮 server，直接取得 client 送出的原始 bytes 來驗證
  - ⏳ **tcpdump 截圖需要你自己執行**：抓封包需要 root 權限（見文末「自己用 tcpdump 驗證」），階段 8 會一起整理成報告素材

---

## 這次建立 / 修改的檔案

| 檔案 | 變更 |
|---|---|
| `include/util.h` | 新增 `CAESAR_DEFAULT_KEY`（3）、`caesar_encrypt()`、`caesar_decrypt()` |
| `lib/util.c` | 實作凱薩位移：只處理可印出 ASCII（32–126），原地修改；金鑰先正規化到 0–94 |
| `server/server.c` | 新增 `-k shift`；收到一行後先解密再執行指令，回應加密後才送出；DEBUG log 會同時印出密文與明文 |
| `client/client.c` | 新增 `-k shift`；送出前加密，收到回覆後解密再印出 |

命令列：

```text
./server/server_naive [-p port] [-k shift] [-v level]
./client/client [-h host] [-p port] [-k shift] [-v level] <command...>
-k 0 = 不加密（明文），用來和加密版對照封包內容
```

### 驗收輸出

**U. 單元測試（直接連結 `libutil.so` 呼叫函式）**

```text
k=3  'ECHO hello' -> 'HFKR#khoor' (expect 'HFKR#khoor') OK
k=3  'xyz~ ' -> '{|}"#' (wrap-around: expect '{|}"#') OK
round-trip/bijection/range over 401 keys (-200..200): OK
k=0 identity: OK, k=95 identity: OK
control chars + UTF-8 untouched: OK
RESULT: ALL PASS
```

對 -200 到 200 每一個金鑰都檢查了以下三點：

1. 95 個字元加密後仍在 32–126 範圍內，所以**永遠不會產生 `\n`**。
2. 加密後沒有重複的字元（一對一對應，**bijection**）。
3. 解密後完全還原（**round-trip**）。

**E1. 端對端（預設 k=3）**

```text
ECHO hello world      → OK hello world
SYSINFO               → OK hostname=GuavasPC;sysname=Linux;release=6.6.114.1-...
HELLO                 → ERR unknown command
QUIT                  → OK bye
"ECHO ~ wrap {|}"     → OK ~ wrap {|}            ← 繞回邊界的字元也正確
```

**W1. 線路上實際傳送的內容（用 `nc -l` 假扮 server，取得 client 送出的原始 bytes）**

```text
$ ./client/client -p 9599 ECHO hello world
client sent: HFKR#khoor#zruog$            ← $ 是 '\n'；空白變成 '#'
```

**W2. Server 回應的原始 bytes（手動送出密文、直接讀取原始回應）**

```text
送出 'HFKR#khoor'（= ECHO hello）  → server raw reply: RN#khoor          （= OK hello）
送出 'V\VLQIR'   （= SYSINFO）     → RN#krvwqdph@JxdydvSF>v|vqdph@Olqx{>uhohdvh@9191...
```

**W3 / K1. 金鑰不一致時無法溝通**

```text
直接送明文 'ECHO hello' 給 k=3 的 server  → raw: HUU#xqnqrzq#frppdqg   （= ERR unknown command）
client -k 0 、server k=3                  → HUU#xqnqrzq#frppdqg     （client 沒解密，看到的是密文）
client -k 5 、server k=3                  → CPP}slilmul}amkk_lb     （兩邊都錯位，變成亂碼）
```

**K2. 其他金鑰**

```text
雙方都 -k 0  → OK plain mode；raw (k=0): OK visible      ← 明文模式，線路上直接看得到內容
雙方都 -k 94 → OK k94 works
```

**C1. UTF-8 中文「不會」被加密**

```text
$ ./client/client -p 9599 "ECHO 你好 abc"
client sent: HFKR#你好#def                ← 「你好」以明文出現在線路上
```

**D. DEBUG log 同時顯示密文與明文**

```text
client: plain: ECHO hi  →  sent cipher: HFKR#kl  →  recv cipher: RN#kl
server: recv cipher: HFKR#kl  →  cmd: ECHO hi  →  reply plain: OK hi  →  sent cipher: RN#kl
```

---

## 為什麼這樣寫（含運作原理）

### 1. 演算法：在 95 個可印出字元上做「模 95」的位移

```text
加密：c' = 32 + ((c - 32 + k) mod 95)
解密：c  = 32 + ((c' - 32 - k + 95) mod 95)
```

1. `c - 32`：把字元從 `' '`(32)～`'~'`(126) 對應到 0～94。
2. `+ k` 之後 `mod 95`：在這 95 個位置的環上往前走 k 步，超過 94 就繞回 0。
3. `+ 32`：再對應回 ASCII。

例：`'~'`(126) 加密 k=3 → 126-32+3 = 97 → 97 mod 95 = 2 → 32+2 = 34 = `'"'`（單元測試的 wrap-around 項目）。

**為什麼選 95 個可印出字元，而不是只處理英文字母**：指令中有空白、`=`、`;`、數字等符號。只轉換 A–Z 的話，這些符號會以明文出現，像 `server_pid=423` 中的結構就一覽無遺。處理全部 95 個字元，連空白也會變成 `#`，比較看不出原本的結構。

### 2. 為什麼加密不會破壞訊息邊界

整個協定的前提是：**一則訊息就是一行，以 `\n` 結尾**（CONTEXT.md B4）。

- `\n` 是 10，不在 32–126 的範圍內，所以**不會被轉換**。
- 範圍內的字元加密後仍然在範圍內（單元測試第 1 點），所以**密文中不會出現 `\n`**。

這就是為什麼處理順序是：

```text
送出：明文內容 → caesar_encrypt → send_line 補上 '\n'
接收：recv_line 以 '\n' 切出一行 → 去掉 '\n' → caesar_decrypt → 明文內容
```

加密只作用在「行的內容」，`\n` 永遠是協定層的東西。`recv_line` 完全不需要知道有加密這回事，所以 `libutil.so` 的網路 I/O 函式一行都不用改。

### 3. 負數取模的陷阱：為什麼要「先加 95」

C 的 `%` 結果的正負號**跟隨被除數**：`-1 % 95 == -1`，而不是數學上的 94。如果直接照公式寫解密：

```c
(c - 32 - k) % 95      /* c='!'(33), k=3 → (1-3) % 95 = -2 → 32 + (-2) = 30（控制字元！） */
```

結果會落到範圍外。解法是先加上 95，保證被除數 ≥ 0：

```c
(c - 32 - k + 95) % 95  /* (1-3+95) % 95 = 93 → 32+93 = 125 = '}' ✅ */
```

程式中的寫法是 `caesar_shift(s, (95 - k) % 95)`：「往回走 k 步」等於「往前走 95-k 步」。加密和解密因此共用同一個函式，被除數也永遠不會是負的。

金鑰本身也可能是負數或大於 94，所以先用 `((k % 95) + 95) % 95` 正規化到 0–94（單元測試涵蓋 -200 到 200）。

### 4. `unsigned char` 的陷阱

```c
unsigned char c = (unsigned char)*s;
if (c < 32 || c > 126) continue;
```

在 x86 上，`char` 預設是 **signed**（-128～127）。UTF-8 中文字的每個 byte 都 ≥ 0x80（例如「你」是 `E4 BD A0`），存在 signed char 中就變成負數（`0xE4` → -28）。

- 如果用 signed 判斷 `c > 126`：-28 不大於 126，但也小於 32，剛好會被跳過。這只是**碰巧**正確。
- 如果寫成 `c - 32` 再取模：(-28 - 32) 是負數，就會算出錯誤的結果。

先轉成 `unsigned char`（0～255），判斷就是明確而正確的，不依賴編譯器對 `char` 的預設。

### 5. 為什麼 UTF-8 中文沒有被加密（C1），以及這代表什麼

依照 CONTEXT.md B5 的規格，範圍外的 byte**不處理**。如果硬要轉換 ≥ 128 的 byte，會打亂 UTF-8 的編碼結構（UTF-8 的每個 byte 都有固定的位元樣式，例如 `110xxxxx`、`10xxxxxx`），對方解密前看到的就是一串無效的編碼。

**後果**：`ECHO 你好` 中的「你好」會以**明文**出現在網路上。這是這個加密設計的另一個弱點，寫報告時可以和第 7 點一起討論。

### 6. `-k 0`：明文模式

金鑰 0（以及 95 的倍數）代表位移 0，等於沒有加密。CONTEXT.md B11 的加分項要求「另抓一次不加密的版本，對比明文外洩」。這裡不需要另外寫一份不加密的程式，只要雙方都用 `-k 0` 執行就可以了（K2）。

### 7. 凱薩加密為什麼不安全（報告重點）

| 弱點 | 說明 | 本專案的實例 |
|---|---|---|
| **金鑰空間極小** | 只有 95 種可能的金鑰（其中 k=0 等於明文），暴力嘗試 94 次一定會找到 | 對 `HFKR#khoor` 依序嘗試 k=1…94，k=3 時就出現 `ECHO hello` |
| **已知明文攻擊** | 只要知道任何一對「明文 ↔ 密文」，馬上就能算出金鑰 | 協定是公開的，回應一定以 `OK ` 或 `ERR` 開頭。看到密文開頭是 `RN#`，`'R' - 'O' = 3`，金鑰就是 3 |
| **頻率分析** | 每個字元永遠被替換成同一個字元，原文的字母出現頻率會原封不動保留在密文中 | 英文中最常見的是空白與 `e`；密文中最常出現的 `#` 很可能就是空白，`#` - ` ` = 3 |
| **結構洩漏** | 長度不變、重複的模式不變 | SYSINFO 回應中的 `;` 全部變成 `>`，`=` 全部變成 `@`，欄位數量一眼就看得出來 |
| **沒有完整性保護** | 攻擊者可以修改密文，接收端無從得知 | 中間人把某個字元 +1，server 就會執行不同的內容 |
| **金鑰無法安全交換** | 雙方必須事先約定 k，沒有安全傳遞金鑰的方法 | `-k` 只能透過命令列手動設定 |

**實務上應該使用 TLS**：用非對稱加密（例如 ECDHE）在不安全的網路上安全地協商出金鑰，用 AES-GCM 或 ChaCha20-Poly1305 這類演算法同時提供加密與完整性驗證，再用憑證確認對方的身分。凱薩加密在這個專案中只是用來**示範**「加密後，封包內容在線路上就看不懂了」。

### 8. 為什麼用 `nc` 驗證「線路上是密文」

用 tcpdump 抓封包需要 root 權限（實測：`tcpdump: lo: You don't have permission to perform this capture on that device`）。改用一個不需要權限、但同樣可靠的方法：

```text
./client/client  ──TCP──▶  nc -l 9599 > wire.bin    （nc 假扮 server，把收到的 bytes 原封不動存起來）
```

`nc` 收到的就是 TCP 傳遞的 payload，和 tcpdump 看到的封包內容相同（tcpdump 只是多了 IP 與 TCP 的標頭）。W1 中看到 `HFKR#khoor#zruog`，證明 client 送出的確實是密文。

### 9. 自己用 tcpdump 驗證（需要 sudo）

在 WSL 開兩個終端機：

```bash
# 終端機 1：在 loopback 介面上抓 port 9000 的封包，-A 以 ASCII 顯示內容
sudo tcpdump -i lo -nn -A -s0 port 9000
```

```bash
# 終端機 2：啟動 server 並送出指令
./server/server_naive &
./client/client ECHO hello world
```

在終端機 1 的 `PSH,ACK` 封包中，應該會看到 `HFKR#khoor#zruog` 和 `RN#khoor#zruog`，而不是明文。雙方都改用 `-k 0` 再做一次，就會看到 `ECHO hello world` 的明文，可以當作對照截圖。

### 10. 這個階段仍刻意保留的限制

| 限制 | 在哪個階段處理 |
|---|---|
| 殭屍行程累積 | 階段 6：R1 |
| 對已斷線的連線寫入會被 SIGPIPE 終止 | 階段 6：R2 |
| 閒置的 client 永久佔用一個子行程 | 階段 6：R3 |
| tcpdump / Wireshark 截圖 | 階段 8 |
