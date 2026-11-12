#!/bin/bash
# =====================================================================
# 強健性對照示範（CONTEXT.md B9）：同一份原始碼的 naive / robust 兩個版本
#
#   用法：./scripts/demo_robust.sh [r1|r2|r3|all]     （預設 all）
#   環境變數：PORT（預設 9700）
#
#   R1 殭屍行程     ：連 5 個 client 後斷線，比較 <defunct> 數量
#   R2 SIGPIPE      ：client 用「一次 write」送出 5 行後立刻關閉、不讀回覆，
#                     server 第二次寫入時對方已 RST → 比較子行程是被殺還是優雅結束
#   R3 閒置 client  ：client 連上後不送任何資料，比較 timeout 後子行程是否還在
#
#   只需要 bash、ps，不需要 root。每段都會印出「預期現象」與「實際觀察」，方便截圖。
# =====================================================================
set -u
cd "$(dirname "$0")/.." || exit 1

PORT=${PORT:-9700}
TIMEOUT=3          # R3 用的 -t 秒數
LOGDIR=$(mktemp -d)
trap 'kill $SRV 2>/dev/null; rm -rf "$LOGDIR"' EXIT
SRV=""

if [ ! -x server/server_naive ] || [ ! -x server/server_robust ]; then
    echo "找不到 server 執行檔，請先執行 make" >&2
    exit 1
fi

hr()    { printf '\n\033[1m%s\033[0m\n' "=================================================================="; }
title() { printf '\033[1;36m%s\033[0m\n' "$*"; }
say()   { printf '\033[33m$ %s\033[0m\n' "$*"; }
note()  { printf '  → %s\n' "$*"; }

start_server() {   # $1 = naive|robust，其餘參數傳給 server
    local v=$1; shift
    ./server/server_"$v" -p "$PORT" "$@" 2>"$LOGDIR/$v.log" &
    SRV=$!
    sleep 0.3
    if ! kill -0 "$SRV" 2>/dev/null; then
        echo "server 啟動失敗："; cat "$LOGDIR/$v.log"; exit 1
    fi
}
stop_server() { kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; SRV=""; }
children()    { ps --ppid "$SRV" -o pid,ppid,stat,cmd; }
show_log()    { sed 's/^\[[^]]*\]//' "$LOGDIR/$1.log" | grep -vE 'listening|robust:|naive:' | sed 's/^/    /'; }

# ---------------------------------------------------------------------
r1() {
    for v in naive robust; do
        hr; title "R1 殭屍行程 — server_$v"
        start_server "$v"
        say "for i in 1..5: ./client/client ECHO hi-\$i"
        for i in 1 2 3 4 5; do ./client/client -p "$PORT" ECHO "hi-$i" >/dev/null; done
        sleep 0.3
        say "ps --ppid $SRV -o pid,ppid,stat,cmd"
        children
        local z; z=$(ps --ppid "$SRV" -o stat= | grep -c Z)
        if [ "$v" = naive ]; then
            note "預期：5 個 Z <defunct>（父行程從不 wait）"
        else
            note "預期：沒有任何子行程（SIGCHLD handler 已用 waitpid 回收）"
        fi
        note "實際：zombie 數量 = $z"
        stop_server
    done
}

# ---------------------------------------------------------------------
r2() {
    for v in naive robust; do
        hr; title "R2 對已斷線的連線寫入（SIGPIPE）— server_$v"
        start_server "$v"
        say "client 以一次 write 送出 5 行指令後立刻 close，不讀任何回覆"
        # 時序（關鍵在「client 先關閉、server 才開始回覆」）：
        #   1. client 一次寫入 5 行，立刻 close → 送出 FIN（此時接收緩衝區是空的，所以是 FIN 不是 RST）
        #   2. server 回第 1 行：send 成功（資料進了核心緩衝區），但 client 的 socket 已關閉 → client 核心回 RST
        #   3. server 回第 2 行：對一條已被 RST 的連線寫入 → 核心送 SIGPIPE
        # 注意：必須是「一次」write。若分很多次寫，server 會在 client 還沒關閉時就回覆，
        # client 關閉時緩衝區裡有未讀資料 → 直接送 RST，server 收到的會是 ECONNRESET 而不是 SIGPIPE。
        exec 3<>"/dev/tcp/127.0.0.1/$PORT"
        printf 'x\nx\nx\nx\nx\n' >&3
        exec 3>&-
        sleep 0.5
        say "server log："
        show_log "$v"
        if [ "$v" = naive ]; then
            say "ps --ppid $SRV -o pid,stat,cmd ；並讀取殭屍的結束狀態"
            children
            local c st
            c=$(ps --ppid "$SRV" -o pid= | head -1 | tr -d ' ')
            if [ -n "$c" ]; then
                # /proc/<pid>/stat 最後一欄 exit_code：與 waitpid 的 status 相同編碼，
                # 低 7 位元 = 終止它的信號編號（13 = SIGPIPE）
                st=$(awk '{print $NF}' "/proc/$c/stat")
                note "子行程 $c 的 exit_code = $st → 被信號 $((st & 127)) 終止（$(kill -l $((st & 127)))）"
            fi
            note "預期：log 只有 connected 與一兩行 cmd，沒有 disconnected — 子行程來不及清理就被殺掉"
        else
            say "ps --ppid $SRV -o pid,stat,cmd"
            children
            note "預期：log 出現 'peer gone while sending (Broken pipe), closing (R2)' 與 disconnected，"
            note "      子行程正常 exit(0) 並被回收"
        fi
        stop_server
    done
}

# ---------------------------------------------------------------------
r3() {
    for v in naive robust; do
        hr; title "R3 閒置 client — server_$v -t $TIMEOUT"
        start_server "$v" -t "$TIMEOUT"
        say "client 連上後什麼都不送（保持連線）"
        exec 4<>"/dev/tcp/127.0.0.1/$PORT"
        sleep 0.3
        say "ps --ppid $SRV -o pid,stat,cmd   （剛連上）"
        children
        say "sleep $((TIMEOUT + 2))"
        sleep $((TIMEOUT + 2))
        say "ps --ppid $SRV -o pid,stat,cmd   （$((TIMEOUT + 2)) 秒後）"
        children
        say "server log："
        show_log "$v"
        if [ "$v" = naive ]; then
            note "預期：子行程仍是 S（阻塞在 recv），永遠不會結束；-t 在 naive 版被忽略"
        else
            note "預期：log 出現 'idle for $TIMEOUT s, closing (R3)'，子行程已結束並被回收"
        fi
        exec 4>&-
        stop_server
    done
}

case "${1:-all}" in
    r1) r1 ;;
    r2) r2 ;;
    r3) r3 ;;
    all) r1; r2; r3 ;;
    *) echo "usage: $0 [r1|r2|r3|all]" >&2; exit 1 ;;
esac
hr
