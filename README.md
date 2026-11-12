# Client–Server Application in C

A concurrent TCP client–server system written in C for Linux. The server forks one child process per client, and the two sides speak a line-based text protocol in which every line is encrypted with a Caesar cipher. Shared code lives in a dynamic library (`libutil.so`). The same server source builds two variants, *naive* and *robust*, so the effect of each robustness mechanism can be demonstrated side by side.

以 C 語言在 Linux 上實作的併發 TCP client–server 系統：server 為每個 client `fork()` 一個子行程，雙方以「一行一則訊息」的文字協定溝通，每一行都經過凱薩加密；共用程式碼編成動態函式庫 `libutil.so`；同一份 server 原始碼編出 naive / robust 兩個版本，用來對照強健性機制的效果。

> Midterm project for *Network System Programming and Security*.
> 「網路系統程式開發與安全」期中專案。

---

## Features

| Feature | Details |
|---|---|
| Concurrency | `fork()` per connection; no threads. Verified with 12 (and 50) simultaneous clients |
| Shared library | `libutil.so` built with `-fPIC -shared`; executables find it through `rpath $ORIGIN/../lib`, with no `LD_LIBRARY_PATH` needed |
| Protocol | One message per line, terminated by `\n`; replies are always a single line; maximum line length 1024 bytes |
| Commands | `SYSINFO` (from `uname()` + `sysinfo()`), `ECHO <text>`, `QUIT` |
| Encryption | Caesar shift over printable ASCII (32–126, mod 95) in both directions; `-k 0` = plaintext for comparison |
| Logging | Two layers: compile time (`make DEBUG=1`) and run time (`LOG_LEVEL` / `-v`); format `[time][level][pid][file:line]` |
| Robustness (`-DROBUST`) | R1 zombie reaping (`SIGCHLD` + `waitpid` loop), R2 `SIGPIPE` handling, R3 idle timeout (`SO_RCVTIMEO`) |
| Packet capture | Captures without `sudo` by using unprivileged user, network and UTS namespaces |

## Requirements

- Linux or WSL2 (`sysinfo()` is Linux-only). Tested on Ubuntu 26.04 / WSL2, gcc 15.2, GNU Make 4.4
- `gcc`, `make` (`sudo apt install build-essential`)
- Optional: `tcpdump` (packet capture) and Wireshark (analysis)

## Build

```bash
make              # lib/libutil.so, server/server_naive, server/server_robust, client/client
make DEBUG=1      # also compile LOG_D debug logging in (rebuilds automatically when flags change)
make clean
ldd server/server_robust    # should list libutil.so => .../lib/libutil.so
```

## Usage

```bash
./server/server_robust [-p port] [-k shift] [-v level] [-t timeout_sec]
./client/client [-h host] [-p port] [-k shift] [-v level] [--hold sec] <command...>
```

| Option | Default | Meaning |
|---|---|---|
| `-p` | `9000` | TCP port |
| `-h` | `127.0.0.1` | Server IPv4 address (client only) |
| `-k` | `3` | Caesar shift (0–94); must be the same on both sides; `0` disables encryption |
| `-v` | `1` | Log level: 0 = error, 1 = info, 2 = debug (overrides `LOG_LEVEL`) |
| `-t` | `30` | Idle timeout in seconds, robust build only; `0` = off |
| `--hold` | `0` | Keep the connection open for N seconds after the reply (for concurrency tests) |

Example:

```text
$ ./server/server_robust &
$ ./client/client ECHO hello world
OK hello world
$ ./client/client SYSINFO
OK hostname=...;sysname=Linux;release=...;machine=x86_64;uptime=...;totalram=...;freeram=...;procs=...;server_pid=...;child_pid=...
$ ./client/client QUIT
OK bye
$ ./client/client HELLO
ERR unknown command
```

On the wire, `ECHO hello world` travels as `HFKR#khoor#zruog` (k = 3).

## Scripts

| Script | Purpose |
|---|---|
| `scripts/stress.sh [N] [HOLD]` | Starts N clients (default 12, each holding 15 s) and checks `ps`, `ss`, the server log and each reply's `child_pid` |
| `scripts/demo_robust.sh [r1\|r2\|r3\|all]` | Side-by-side naive vs. robust demonstration of R1, R2 and R3 |
| `scripts/capture.sh` | Captures encrypted, plaintext and FIN/RST pcaps into `docs/capture/` without root |

## Robustness: naive vs. robust

| | `server_naive` | `server_robust` |
|---|---|---|
| R1 Zombies | `<defunct>` children accumulate | `SIGCHLD` handler loops `waitpid(-1, NULL, WNOHANG)`, `SA_RESTART` |
| R2 Write to a reset peer | Child is killed by `SIGPIPE` with no log | `SIGPIPE` ignored; `send` returns `EPIPE` and the child exits cleanly |
| R3 Idle client | Child blocks in `recv` forever | `SO_RCVTIMEO`; the connection is closed after the timeout |

## Project Layout

```text
├── Makefile
├── CONTEXT.md            # specification and development plan
├── include/util.h        # public interface of libutil.so
├── lib/util.c            # Caesar, send_all / recv_line / send_line, logging
├── server/server.c       # fork-per-client server (naive / robust via -DROBUST)
├── client/client.c
├── scripts/              # stress.sh, demo_robust.sh, capture.sh
└── docs/
    ├── README.md         # index of report materials and screenshot checklist
    ├── dev-log/          # per-stage notes: what was built and why it works (stage-0 … stage-8)
    ├── capture/          # pcaps + text decodes
    └── results/          # build, stress-test and robustness-demo outputs
```

The development logs in [`docs/dev-log/`](docs/dev-log/) explain each design decision and the underlying system and network mechanisms (written in Traditional Chinese).
各階段的設計理由與運作原理記錄在 [`docs/dev-log/`](docs/dev-log/)（繁體中文）。

## Security Note

The Caesar cipher here is for **demonstration only**. It has just 95 possible keys, preserves character frequencies and message structure, and offers no integrity protection, so it can be broken instantly by brute force, known-plaintext attacks (every reply starts with `OK `) or frequency analysis. Real systems should use TLS.

凱薩加密僅供教學示範，可被暴力破解與頻率分析；實務上應使用 TLS。

Hostnames in the published materials are replaced with the label `REDACTED-HOSTNAME` (see [`docs/README.md`](docs/README.md)).
公開素材中的主機名稱已以標籤 `REDACTED-HOSTNAME` 取代。

---

## AI Usage Statement／AI 使用聲明

This project was developed with the assistance of **Claude** (Anthropic), used through Claude Code.
本專案在開發過程中使用了 Anthropic 的 **Claude**（透過 Claude Code）協助。

### Claude's contributions／Claude 負責的部分

- **Implementation**: all source code, including `libutil.so` (Caesar cipher, `send_all` / `recv_line` / `send_line`, two-layer logging), the fork-per-client server with its `ROBUST` mechanisms, the client, and the `Makefile`.
  **程式實作**：全部原始碼，包括 `libutil.so`、server（含強健性機制）、client 與 `Makefile`。
- **Scripts**: `stress.sh`, `demo_robust.sh` and `capture.sh`.
  **腳本**：壓測、強健性對照、封包擷取腳本。
- **Testing and debugging**: building and testing every stage in WSL, writing test cases, and diagnosing problems found along the way (e.g. reproducing `SIGPIPE` reliably, empty packet captures).
  **測試與除錯**：在 WSL 中逐階段編譯、測試，撰寫測試案例，排除開發中遇到的問題。
- **Documentation**: the per-stage development logs (`docs/dev-log/`), the materials index (`docs/README.md`), this README, packet-capture analysis and the collected outputs in `docs/results/`.
  **文件**：各階段開發紀錄、素材索引、本 README、封包分析與測試輸出整理。
- **Version control**: commit messages and commits for each stage.
  **版本控制**：各階段的 commit。

### My contributions／我負責的部分

- **Researching and understanding the concepts**: client–server model, sockets and TCP, `fork()` and file descriptors, signals and zombie processes, shared libraries, conditional compilation, packet analysis, and why the Caesar cipher is insecure.
  **研究與理解概念**：client–server 模型、socket 與 TCP、`fork()` 與檔案描述符、信號與殭屍行程、動態函式庫、條件編譯、封包分析，以及凱薩加密為何不安全。
- **Practicing the client–server application**: building and running the server and client, operating the scripts, observing the behaviour (concurrency, zombies, `SIGPIPE`, timeouts, packet captures), and producing the screenshots and demonstration.
  **練習操作 client–server application**：自行編譯、執行 server 與 client，操作各個腳本，觀察併發、殭屍行程、SIGPIPE、逾時與封包等現象，並製作截圖與示範。
