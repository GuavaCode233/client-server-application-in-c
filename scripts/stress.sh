#!/bin/bash
# =====================================================================
# 併發壓測：同時啟動 N 個 client，每個連線後保持 HOLD 秒（CONTEXT.md B10）
#
#   用法：./scripts/stress.sh [N] [HOLD]          預設 N=12、HOLD=15
#   環境變數：
#     PORT     server 埠號（預設 9000）
#     VARIANT  robust | naive（預設 robust）
#     CMD      每個 client 送出的指令（預設 SYSINFO，可從回覆看到 child_pid）
#
#   會收集三種證明素材（全部不需要 root）：
#     1. server log：N 筆不同子行程 pid 的連線紀錄
#     2. ps --ppid <server_pid> -o pid,stat,cmd：N 個子行程
#     3. ss -tn state established '( sport = :PORT )'：N 條連線
#   想把結果存成報告素材：./scripts/stress.sh | tee docs/stress.txt
# =====================================================================
set -u
cd "$(dirname "$0")/.." || exit 1

N=${1:-12}
HOLD=${2:-15}
PORT=${PORT:-9000}
VARIANT=${VARIANT:-robust}
CMD=${CMD:-SYSINFO}

SERVER=./server/server_$VARIANT
if [ ! -x "$SERVER" ] || [ ! -x ./client/client ]; then
    echo "找不到執行檔，請先執行 make" >&2
    exit 1
fi

WORK=$(mktemp -d)
SRV=""
CPIDS=()
cleanup() {
    for p in "${CPIDS[@]}"; do kill "$p" 2>/dev/null; done
    [ -n "$SRV" ] && kill "$SRV" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

hr()   { printf '\n\033[1m%s\033[0m\n' "------------------------------------------------------------------"; }
say()  { printf '\033[33m$ %s\033[0m\n' "$*"; }
pass() { printf '  \033[32m✔ %s\033[0m\n' "$*"; }
fail() { printf '  \033[31m✘ %s\033[0m\n' "$*"; FAILED=1; }
FAILED=0

# --- 1. 啟動 server -----------------------------------------------------
# -t 設成比 HOLD 長，避免 robust 版的 R3 閒置逾時在壓測途中把連線踢掉
"$SERVER" -p "$PORT" -t $((HOLD + 30)) 2>"$WORK/server.log" &
SRV=$!
sleep 0.3
if ! kill -0 "$SRV" 2>/dev/null; then
    echo "server 啟動失敗："; cat "$WORK/server.log"; exit 1
fi
hr; echo "server_$VARIANT pid=$SRV port=$PORT；同時啟動 $N 個 client，每個保持 $HOLD 秒，指令：$CMD"

# --- 2. 同時啟動 N 個 client（全部放到背景）----------------------------
T0=$(date +%s.%N)
for i in $(seq 1 "$N"); do
    # shellcheck disable=SC2086
    ./client/client -p "$PORT" --hold "$HOLD" $CMD >"$WORK/client_$i.out" 2>"$WORK/client_$i.err" &
    CPIDS+=($!)
done
sleep 1   # 讓所有連線建立完成、server 端完成 fork

# --- 3. 收集證明素材（連線都還在的時候）--------------------------------
hr; say "ps --ppid $SRV -o pid,stat,cmd"
ps --ppid "$SRV" -o pid,stat,cmd
NCHILD=$(ps --ppid "$SRV" -o pid= | wc -l)

hr; say "ss -tn state established '( sport = :$PORT )'"
ss -tn state established "( sport = :$PORT )"
NCONN=$(ss -Htn state established "( sport = :$PORT )" | wc -l)

hr; say "server log（connected 紀錄）"
grep 'connected (child pid=' "$WORK/server.log" | sed 's/^\[[^]]*\]//'
NLOGPID=$(grep -o 'connected (child pid=[0-9]*' "$WORK/server.log" | sort -u | wc -l)

hr; say "各 client 收到的回覆（節錄 child_pid）"
for i in $(seq 1 "$N"); do
    printf '  client %-2d %s\n' "$i" "$(grep -o 'server_pid=[0-9]*;child_pid=[0-9]*' "$WORK/client_$i.out" || head -c 60 "$WORK/client_$i.out")"
done
NREPLYPID=$(cat "$WORK"/client_*.out | grep -o 'child_pid=[0-9]*' | sort -u | wc -l)

# --- 4. 等所有 client 結束 ----------------------------------------------
hr; echo "等待 client 保持連線 $HOLD 秒後結束……"
CFAIL=0
for p in "${CPIDS[@]}"; do wait "$p" || CFAIL=$((CFAIL + 1)); done
CPIDS=()
T1=$(date +%s.%N)
sleep 0.5
NAFTER=$(ps --ppid "$SRV" -o pid= | wc -l)
NZOMBIE=$(ps --ppid "$SRV" -o stat= | grep -c Z)
NDISC=$(grep -c ' disconnected' "$WORK/server.log")

# --- 5. 判定 -----------------------------------------------------------
hr; echo "結果"
[ "$NCHILD"    -ge "$N" ] && pass "同時存在的子行程：$NCHILD"          || fail "同時存在的子行程：$NCHILD（預期 ≥ $N）"
[ "$NCONN"     -ge "$N" ] && pass "ESTABLISHED 連線：$NCONN"           || fail "ESTABLISHED 連線：$NCONN（預期 ≥ $N）"
[ "$NLOGPID"   -ge "$N" ] && pass "log 中不同的子行程 pid：$NLOGPID"   || fail "log 中不同的子行程 pid：$NLOGPID（預期 ≥ $N）"
if [ "$CMD" = SYSINFO ]; then
    [ "$NREPLYPID" -ge "$N" ] && pass "回覆中不同的 child_pid：$NREPLYPID" || fail "回覆中不同的 child_pid：$NREPLYPID（預期 ≥ $N）"
fi
[ "$CFAIL"     -eq 0 ]    && pass "client 全部成功結束（exit 0）"      || fail "$CFAIL 個 client 失敗（見 $WORK/client_*.err）"
[ "$NDISC"     -ge "$N" ] && pass "log 中的 disconnected：$NDISC"      || fail "log 中的 disconnected：$NDISC（預期 ≥ $N）"
printf '  總耗時 %s 秒（若是逐一服務會需要 %d × %d = %d 秒）\n' \
    "$(awk -v a="$T0" -v b="$T1" 'BEGIN { printf "%.1f", b - a }')" "$N" "$HOLD" $((N * HOLD))
if [ "$VARIANT" = robust ]; then
    [ "$NAFTER" -eq 0 ] && pass "結束後剩餘子行程：0（R1 已回收）" || fail "結束後剩餘子行程：$NAFTER"
else
    echo "  （naive 版）結束後殭屍行程：$NZOMBIE（預期 = $N，見 R1）"
fi
hr
exit "$FAILED"
