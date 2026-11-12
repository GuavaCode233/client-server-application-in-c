# 報告素材索引

這個目錄收集期中報告需要的所有素材。最終報告：**書面 PDF，英文為主、中文為輔**（CONTEXT.md Part D 第 11 點）。

## 1. 已經產生的素材

| 素材 | 檔案 | 怎麼重新產生 |
|---|---|---|
| 每階段開發紀錄（設計理由、運作原理、測試輸出） | [`dev-log/stage-0.md`](dev-log/stage-0.md) ～ [`stage-8.md`](dev-log/stage-8.md) | — |
| 編譯、`ldd`、rpath、兩層 log、ROBUST 符號 | [`results/build.txt`](results/build.txt) | 見 stage-8.md |
| 12 個 client 併發壓測（`ps` / `ss` / log / child_pid） | [`results/stress.txt`](results/stress.txt) | `./scripts/stress.sh \| tee docs/results/stress.txt` |
| R1 / R2 / R3 的 naive vs. robust 對照 | [`results/demo_robust.txt`](results/demo_robust.txt) | `./scripts/demo_robust.sh all` |
| 封包：加密（k=3） | [`capture/capture.pcap`](capture/capture.pcap)、[`.txt`](capture/capture.txt) | `./scripts/capture.sh` |
| 封包：不加密（k=0），明文外洩對照 | [`capture/capture_plain.pcap`](capture/capture_plain.pcap)、[`.txt`](capture/capture_plain.txt) | 同上 |
| 封包：FIN vs. RST 斷線對照 | [`capture/capture_rst.pcap`](capture/capture_rst.pcap)、[`.txt`](capture/capture_rst.txt) | 同上 |

> `.txt` 檔中的 `results/` 輸出已經去除終端機顏色碼。截圖時請直接在終端機執行腳本，畫面會有顏色。

## 2. 需要自己截圖的項目（對應 CONTEXT.md Part F）

所有指令都在專案根目錄、WSL Ubuntu 中執行。執行之前先 `make`。

### A. 10+ 併發證明（1–3 張）

```bash
./scripts/stress.sh 12 15
```

截取三個區塊：`ps --ppid`（12 個子行程）、`ss -tn`（12 條連線）、最後的「結果」（全部 ✔，以及 15.0 秒 vs. 180 秒）。

### B. R1 / R2 / R3 對照（共 ≥ 3 組，每組 naive 與 robust 各一張）

```bash
./scripts/demo_robust.sh r1
./scripts/demo_robust.sh r2
./scripts/demo_robust.sh r3
```

每組都要截到：執行的指令（黃字 `$ ...`）、`ps` 的結果、server log，以及 `→ 預期 / 實際` 那幾行。

| 組別 | naive 的重點畫面 | robust 的重點畫面 |
|---|---|---|
| R1 | 5 個 `Z+ <defunct>` | `ps` 是空的 |
| R2 | log 停在 `cmd: u`；`exit_code = 13 → (PIPE)` | `peer gone while sending (Broken pipe)` + `disconnected` |
| R3 | 5 秒後子行程仍是 `S+` | `idle for 3 s, closing (R3)` |

### C. 封包分析（Wireshark，在 Windows 上操作）

1. 安裝 Wireshark（Windows 版，預設選項）。
2. 開啟 `D:\Coding\0_Projects\school_assignments\client-server-application-in-c\docs\capture\capture.pcap`。
3. 截圖：
   - **三向交握**：封包清單的前三行 `[SYN]`、`[SYN, ACK]`、`[ACK]`。
   - **密文 payload**：點選第一個 `[PSH, ACK]`，在下方的位元組窗格中看到 `HFKR#khoor#zruog`。
   - **Follow TCP Stream**：在任一封包上按右鍵 → Follow → TCP Stream。看到的是紅色（client）的 `HFKR#khoor#zruog` 和藍色（server）的 `RN#khoor#zruog`。
   - **斷線**：`[FIN, ACK]` 的封包。
4. 開啟 `capture_plain.pcap`，對同一條 stream 做 Follow TCP Stream → 看到 `ECHO hello world` / `OK hello world` 的明文。和上一張並排，就是「明文外洩」的對照。
5. 開啟 `capture_rst.pcap`，截第二條連線最後的 `[RST, ACK]`（紅色那行）。

> 也可以不用 Wireshark，直接用終端機截圖：`tcpdump -nn -A -r docs/capture/capture.pcap`（讀檔不需要 root）。

### D. 其他（選用，但能加分）

| 項目 | 指令 |
|---|---|
| `.so` 被正確載入 | `ldd server/server_robust` |
| 兩層 log | `make DEBUG=1 && LOG_LEVEL=2 ./server/server_robust`，再開一個終端機 `./client/client ECHO hi` |
| 編譯期開關確實移除了程式碼 | `make && strings server/server_naive \| grep -c "debug log"`（0）vs. `make DEBUG=1` 後再執行一次（1） |

## 3. 2 分鐘說明影片大綱（加分項）

| 時間 | 畫面 | 旁白重點 |
|---|---|---|
| 0:00–0:15 | 架構圖（client → TCP → server 父行程 → fork → 子行程） | 專案目標：fork 併發 server、`.so` 共用函式庫、凱薩加密、強健性機制 |
| 0:15–0:35 | `make`、`ldd server/server_robust` | 一個 Makefile 編出 `.so` 與兩個 server 版本；rpath 讓執行檔找到 `.so` |
| 0:35–0:55 | `./client/client SYSINFO`、`ECHO`、`QUIT` | 文字行協定，`\n` 是唯一的訊息邊界 |
| 0:55–1:15 | `./scripts/stress.sh 12 5` | 12 個子行程、12 條連線，總耗時 ≈ 5 秒而不是 60 秒 |
| 1:15–1:40 | `./scripts/demo_robust.sh r1`（或 r2） | 同一份原始碼，用 `-DROBUST` 切換；naive 留下殭屍 / 被 SIGPIPE 殺掉，robust 正常 |
| 1:40–2:00 | Wireshark Follow TCP Stream：密文 vs. 明文 | 線路上是密文；但凱薩加密只有 95 種金鑰，實務上應該使用 TLS |

## 4. 敏感資訊的標籤處理

這個 repo 是公開的，所以所有素材中的**主機名稱**都已經換成標籤 **`REDACTED-HOSTNAME`**：

| 位置 | 處理方式 |
|---|---|
| `capture/*.pcap`、`capture/*.txt` | `capture.sh` 在私有的 UTS 命名空間中把主機名稱設成標籤後才擷取，封包本身完整、未經修改（見 [stage-8.md](dev-log/stage-8.md) 第 6 點）。加密版中的密文也已經是標籤的密文 `UHGDFWHG0KRVWQDPH` |
| `dev-log/*.md` | 以文字取代 |

**截圖時請注意**：你自己在終端機執行 `./client/client SYSINFO`、`stress.sh` 時，畫面會顯示**真正的主機名稱**。放進報告前請遮蔽，或改用 `./scripts/capture.sh` 產生的素材。

沒有遮蔽的資訊：核心版本、記憶體大小、行程數、pid、`127.0.0.1`。這些不能用來識別個人，而且是 SYSINFO 要示範的內容。
