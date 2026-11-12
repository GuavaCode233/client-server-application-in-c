#!/bin/bash
# =====================================================================
# 封包擷取（CONTEXT.md B11）：產生可用 Wireshark 開啟的 pcap 與文字版解碼
#
#   用法：./scripts/capture.sh [輸出目錄]          （預設 docs/capture）
#
#   會產生：
#     capture.pcap         加密（k=3）：ECHO / SYSINFO / QUIT，各自一條連線
#     capture_plain.pcap   不加密（k=0）：同樣的指令，對照「明文外洩」
#     capture_rst.pcap     斷線對照：正常 QUIT（FIN）vs. 關閉時仍有未讀資料（RST）
#     *.txt                tcpdump -nn -A 的文字解碼（含 payload），可直接貼進報告
#
#   權限：
#     抓封包需要 CAP_NET_RAW。本腳本優先使用「無特權的使用者命名空間 + 網路命名空間」
#     （unshare -rn）：在私有的網路命名空間中我們是 root，可以抓它自己的 loopback，
#     不需要 sudo、不會改動系統設定，也不會抓到其他程式的流量。
#     若系統不允許 unshare，改用：sudo ./scripts/capture.sh
#
#   敏感資訊：
#     SYSINFO 回應含主機名稱。腳本另外建立一個私有的 UTS 命名空間（unshare -u），
#     在其中把主機名稱設為標籤 CAPTURE_HOSTNAME（預設 "REDACTED-HOSTNAME"），
#     所以 pcap 中不會出現真實主機名稱，而封包本身仍是完整、未經修改的。
#     只影響這個命名空間，系統真正的主機名稱不會改變。
#     要保留真實主機名稱：CAPTURE_HOSTNAME= ./scripts/capture.sh
# =====================================================================
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)
OUT=${1:-docs/capture}
case "$OUT" in /*) ;; *) OUT="$ROOT/$OUT" ;; esac
PORT=9000

for b in server/server_robust client/client; do
    [ -x "$b" ] || { echo "找不到 $b，請先執行 make" >&2; exit 1; }
done
command -v tcpdump >/dev/null || { echo "請先安裝 tcpdump：sudo apt install tcpdump" >&2; exit 1; }

# --- 進入私有命名空間（網路 + UTS），再執行自己一次 ------------------------
# CAPTURE_IN_NS 標記「已經在命名空間裡」，避免無限重新執行。
# 即使以 sudo 執行，也一定進入新的 UTS 命名空間：之後的 hostname 指令才不會改到真實系統。
export CAPTURE_HOSTNAME=${CAPTURE_HOSTNAME-REDACTED-HOSTNAME}
if [ -z "${CAPTURE_IN_NS:-}" ]; then
    export CAPTURE_IN_NS=1
    if [ "$(id -u)" -eq 0 ]; then
        exec unshare -nu "$0" "$OUT"
    fi
    if unshare -rnu true 2>/dev/null; then
        exec unshare -rnu "$0" "$OUT"
    fi
    echo "無法建立使用者命名空間，請改用：sudo $0 $*" >&2
    exit 1
fi

# 在新的網路命名空間中，lo 預設是關閉的
ip link set lo up 2>/dev/null || true
mkdir -p "$OUT"

# 在私有的 UTS 命名空間中替換主機名稱（server 的 uname() 會讀到這個值）
# hostname 指令呼叫 sethostname()，只改變這個 UTS 命名空間的主機名稱。
# 標籤必須符合主機名稱格式（英數字與 '-'），所以不能用 "<HOSTNAME>" 這種寫法。
if [ -n "$CAPTURE_HOSTNAME" ]; then
    hostname "$CAPTURE_HOSTNAME" || { echo "無法設定主機名稱標籤" >&2; exit 1; }
    echo "（主機名稱在這次擷取中顯示為 $(uname -n)）"
fi

TD=""
SRV=""
cleanup() {
    [ -n "$SRV" ] && kill "$SRV" 2>/dev/null
    [ -n "$TD" ] && kill -INT "$TD" 2>/dev/null
    rm -f "$ERRF"
}
trap cleanup EXIT

ERRF=$(mktemp)
start_capture() {   # $1 = pcap 檔名
    # -Z root：不要切換成 tcpdump 使用者（命名空間中沒有對應的 uid，切換會失敗）
    # -s0    ：抓完整封包，不截斷 payload
    # -U     ：每個封包立刻寫入檔案（不在記憶體中累積）
    : >"$ERRF"   # 先清空：避免讀到上一輪殘留的 "listening on" 而誤判已就緒
    tcpdump -Z root -i lo -nn -s0 -U -w "$OUT/$1" "tcp port $PORT" 2>"$ERRF" &
    TD=$!
    for _ in $(seq 50); do grep -q 'listening on' "$ERRF" && break; sleep 0.1; done
    # 實測：「listening on」印出時擷取未必已完全生效，再多等 0.5 秒才穩定
    sleep 0.5
}
stop_capture() {    # $1 = pcap 檔名
    sleep 0.3
    kill -INT "$TD"; wait "$TD" 2>/dev/null; TD=""
    # 讀檔同樣要 -Z root，否則 tcpdump 會嘗試切換使用者而失敗
    local n
    n=$(tcpdump -Z root -nn -r "$OUT/$1" 2>/dev/null | wc -l)
    if [ "$n" -eq 0 ]; then
        echo "  ✘ $1 沒有抓到任何封包："; sed 's/^/    /' "$ERRF"
        exit 1
    fi
    # 文字版解碼：-A 以 ASCII 顯示 payload，報告可以直接引用
    tcpdump -Z root -nn -A -r "$OUT/$1" 2>/dev/null > "$OUT/${1%.pcap}.txt"
    echo "  → $OUT/$1（$n 個封包）"
}
start_server() {    # $@ = server 參數
    ./server/server_robust -p "$PORT" "$@" 2>/dev/null &
    SRV=$!
    sleep 0.3
}
stop_server() { kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; SRV=""; }

run_cmds() {        # $1 = k
    ./client/client -p "$PORT" -k "$1" ECHO hello world
    ./client/client -p "$PORT" -k "$1" SYSINFO
    ./client/client -p "$PORT" -k "$1" QUIT
}

echo "== 1. 加密（k=3）"
start_capture capture.pcap
start_server -k 3
run_cmds 3 | sed 's/^/  client: /'
stop_server
stop_capture capture.pcap

echo "== 2. 不加密（k=0）"
start_capture capture_plain.pcap
start_server -k 0
run_cmds 0 | sed 's/^/  client: /'
stop_server
stop_capture capture_plain.pcap

echo "== 3. 斷線方式對照：FIN vs. RST"
start_capture capture_rst.pcap
start_server -k 3
# (a) 正常結束：送 QUIT（k=3 → TXLW），讀回 OK bye，雙方以 FIN 關閉
./client/client -p "$PORT" -k 3 QUIT | sed 's/^/  client (a): /'
# (b) QUIT 之後還多送一行：server 回 OK bye 後關閉，但接收緩衝區中仍有未讀的那一行
#     → Linux 以 RST 取代 FIN，告訴對方「有資料被丟棄了」
exec 3<>"/dev/tcp/127.0.0.1/$PORT"
printf 'TXLW\nHFKR#diwhu\n' >&3
read -r -t 2 r <&3 && echo "  client (b): raw reply $r"
sleep 0.3
exec 3>&-
stop_server
stop_capture capture_rst.pcap

echo "完成。用 Wireshark 開啟 pcap，或直接查看同名的 .txt 檔。"
