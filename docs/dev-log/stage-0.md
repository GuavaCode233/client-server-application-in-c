# 階段 0：目錄 + Makefile + 空的 libutil.so

- **驗收標準**：`make` 成功、`ldd` 看到 `.so`
- **驗收結果**：✅ 通過（WSL Ubuntu 26.04、gcc 15.2.0、GNU Make 4.4.1）
- **Commit**：`dcb0896`

---

## 這次建立的檔案

| 檔案 | 內容 |
|---|---|
| `Makefile` | 目標 `all`、`lib`、`server_naive`、`server_robust`、`client`、`clean`；支援 `DEBUG=1`、`PORT=` |
| `include/util.h` | `libutil.so` 的公開介面，目前只有 `util_version()` |
| `lib/util.c` | `util_version()` 的實作，編成 `lib/libutil.so` |
| `server/server.c` | 印出自己是 naive 還是 robust 版本，以及函式庫版本 |
| `client/client.c` | 印出函式庫版本 |
| `scripts/`、`docs/` | 空目錄（用 `.gitkeep` 讓 git 追蹤） |
| `.gitignore` | 排除 `.so`、執行檔、`.pcap` 等產物 |
| `.gitattributes` | 強制所有文字檔使用 LF 換行 |

### 驗收輸出

```text
$ make
gcc -std=gnu11 -Wall -Wextra -g -Iinclude -DDEFAULT_PORT=9000 -fPIC -shared -o lib/libutil.so lib/util.c
gcc ... -o server/server_naive  server/server.c -Llib -Wl,-rpath,'$ORIGIN/../lib' -lutil
gcc ... -DROBUST -o server/server_robust server/server.c -Llib -Wl,-rpath,'$ORIGIN/../lib' -lutil
gcc ... -o client/client client/client.c -Llib -Wl,-rpath,'$ORIGIN/../lib' -lutil

$ ldd server/server_robust
    libutil.so => .../server/../lib/libutil.so
    libc.so.6 => /usr/lib/x86_64-linux-gnu/libc.so.6

$ ./server/server_naive
server (naive) using libutil 0.0 (stage 0)
$ ./server/server_robust
server (robust) using libutil 0.0 (stage 0)

$ readelf -d client/client | grep -E 'NEEDED|RUNPATH'
 (NEEDED)   Shared library: [libutil.so]
 (RUNPATH)  Library runpath: [$ORIGIN/../lib]
```

另外在 `/tmp` 用絕對路徑執行 `client/client` 也能正常跑，這表示 rpath 跟「目前在哪個目錄」無關。

---

## 為什麼這樣寫（含運作原理）

### 1. 從原始碼到執行：靜態連結 vs. 動態連結

C 程式變成可以跑的程式，要經過三個階段：

1. **編譯（compile）**：`.c` → 機器碼（目的檔）。這時程式碼只知道「有一個叫 `util_version` 的函式」，因為 `util.h` 有宣告它，但不知道它在哪裡。
2. **連結（link）**：把目的檔和函式庫組合成執行檔。
   - **靜態函式庫 `.a`**：把函式的機器碼直接**複製進**執行檔。
   - **動態函式庫 `.so`**：執行檔裡只記一筆「我需要 `libutil.so`」，也就是 ELF 的 `NEEDED` 欄位，程式碼本身不複製進來。
3. **載入（load）**：執行程式時，Linux 的**動態連結器** `ld-linux-x86-64.so.2` 先讀出 `NEEDED` 清單，找到每個 `.so` 並對應（map）到記憶體，再把函式呼叫指到正確的位址，最後才跳進 `main()`。

`ldd` 做的事，就是模擬第 3 步並列出每個 `.so` 被找到的位置，所以用它來驗收。

### 2. 為什麼 `.so` 要加 `-fPIC -shared`

- `-shared`：輸出共享函式庫，不是執行檔（沒有 `main` 的進入點）。
- `-fPIC`（Position Independent Code，位置無關程式碼）：`.so` 每次被載入的記憶體位址都不一樣（每個行程不同，還有 ASLR 隨機化）。PIC 產生的程式碼不寫死絕對位址，而是用「相對於目前指令的位移」和 GOT（Global Offset Table）間接存取。這樣同一份 `.so` 放在任何位址都能執行，多個行程也能共用同一份實體記憶體。

### 3. 為什麼「空的」.so 還是要放一個 `util_version()`

Ubuntu 的 gcc 預設會傳 `--as-needed` 給連結器，意思是「執行檔沒有真的用到的函式庫，就不寫進 `NEEDED`」。如果 `libutil.so` 是完全空的，或是 server 沒有呼叫裡面任何函式，`-lutil` 會被默默丟掉，`ldd` 就看不到它，階段 0 無法驗收。所以放一個真正會被呼叫的函式，確保這條連結鏈是真的通的。

### 4. 動態連結器去哪裡找 `.so`？為什麼用 `rpath` + `$ORIGIN`

動態連結器找 `libutil.so` 的順序大致如下：

1. 執行檔內的 `RUNPATH` 或 `RPATH`（編譯時用 `-Wl,-rpath,...` 寫入）
2. 環境變數 `LD_LIBRARY_PATH`
3. 系統快取 `/etc/ld.so.cache`，以及 `/lib`、`/usr/lib` 等預設路徑

`libutil.so` 不在系統路徑裡。如果不做任何設定，執行時會出現 `cannot open shared object file`。可以選的解法有：

| 方法 | 缺點 |
|---|---|
| 每次執行前 `export LD_LIBRARY_PATH=...` | 忘記設就跑不起來；換電腦、換終端機都要重設 |
| 把 `.so` 複製到 `/usr/lib` | 需要 root，還會污染系統 |
| **`-Wl,-rpath,'$ORIGIN/../lib'`** ✅ | 無 |

`$ORIGIN` 是動態連結器認得的特殊字串，意思是「這個執行檔所在的目錄」。`server/server_robust` 會去 `server/../lib/` 找，也就是專案的 `lib/`。不論從哪個目錄啟動、專案搬到哪台電腦，都找得到。這符合 Part D 第 8 點「在另一台乾淨的電腦上用 `make` 一次編譯、直接執行」。

**跳脫字元的細節**：在 Makefile 中，`$` 是 make 變數的符號，所以要寫成 `$$ORIGIN`，make 才會輸出一個字面上的 `$`。外層的單引號 `'...'` 則是防止 shell 把 `$ORIGIN` 當成環境變數展開成空字串。從 `readelf` 的輸出可以確認，最後寫進執行檔的就是字面上的 `$ORIGIN/../lib`。

### 5. 同一份 `server.c` 編出兩個版本：條件編譯

```c
#ifdef ROBUST
    const char *variant = "robust";
#else
    const char *variant = "naive";
#endif
```

`#ifdef` 在**前置處理階段**（編譯之前）就決定要留下哪一段程式碼。Makefile 的 `server_robust` 規則多加了 `-DROBUST`，等於在檔案開頭寫了 `#define ROBUST`。所以兩個執行檔來自同一份原始碼，機器碼卻不同。階段 6 會用這個機制做「有 / 無強健性機制」的對照，這次先印出版本名稱，確認巨集真的有傳進去。

`DEBUG=1` 也是同樣的原理（加上 `-DENABLE_DEBUG`），階段 2 會用它控制 `LOG_D` 是否被編進執行檔。

### 6. Makefile 的相依關係

```make
server/server_robust: server/server.c $(HDR) $(LIB)
	$(CC) $(CFLAGS) -DROBUST -o $@ $< $(LDFLAGS) $(LDLIBS)
```

- 冒號左邊是**目標**，右邊是**相依檔**。make 會比較修改時間：只要任何一個相依檔比目標新，就重新執行下面的指令。
- 把 `$(HDR)`（標頭檔）列為相依，是因為改了 `util.h` 而沒重編，介面不一致時可能出現難以追查的錯誤。
- 把 `$(LIB)` 列為相依，可以確保**先編好 `.so` 再連結執行檔**，否則 `-lutil` 會找不到。
- 自動變數：`$@` 是目標名稱，`$<` 是第一個相依檔。
- 指令行**必須用 Tab 縮排**，不能用空白，這是 make 的語法規定。
- `-Llib` 是「連結時」去 `lib/` 找 `libutil.so`；`-rpath` 是「執行時」去哪裡找。兩者是不同階段的事。

### 7. 為什麼強制 LF 換行

Windows 的換行是 `\r\n`（CRLF），Linux 是 `\n`（LF）。這台電腦的 git 設了 `core.autocrlf=true`，在 Windows checkout 時會把檔案轉成 CRLF，但程式是在 WSL 裡編譯和執行的：

- shell 腳本第一行變成 `#!/bin/bash\r`，會出現 `bad interpreter` 或 `$'\r': command not found`。
- Makefile 的變數值後面多了 `\r`，造成很奇怪的錯誤。

`.gitattributes` 設定 `* text=auto eol=lf`，不管本機的 git 設定為何，這個 repo 的文字檔一律用 LF。
