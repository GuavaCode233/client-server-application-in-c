/*
 * 階段 7：client，指令與回應都以凱薩加密傳輸，可用 --hold 保持連線
 *
 * 流程：socket → connect → 加密指令並送出一行 → 讀回一行 → 解密 → 印出
 *       →（--hold N：保持連線 N 秒）→ close
 *
 * 用法：./client [-h host] [-p port] [-k shift] [-v level] [--hold sec] <command...>
 *   命令列剩下的參數以空白串成一行送出（send_line 會補 '\n'）。
 *   例：./client -p 9000 ECHO hello   → 線路上是 "HFKR#khoor\n"（k=3）
 *   -k 必須與 server 相同；-k 0 = 不加密。
 *   --hold 用於併發測試：讓連線（以及 server 端對應的子行程）存活一段時間，
 *   才能同時觀察到多個連線（CONTEXT.md B10）。
 *
 * 輸出分流：server 的回覆印到 stdout；log 一律寫到 stderr。
 * 這樣 `./client ... > out.txt` 只會存到回覆，不會混進 log。
 */
#include <arpa/inet.h>   /* inet_pton, htons */
#include <errno.h>
#include <getopt.h>      /* getopt_long：支援 --hold 這種長選項 */
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "util.h"

#ifndef DEFAULT_PORT
#define DEFAULT_PORT 9000
#endif

static int parse_int(const char *s, int max)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (s[0] == '\0' || *end != '\0' || v < 0 || v > max)
        return -1;
    return (int)v;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-h host] [-p port] [-k shift(0-94)] [-v level(0-2)] [--hold sec]"
            " <command...>\n", prog);
}

/* 長選項只有一個 --hold；回傳值 'H' 用來和短選項區分 */
static const struct option long_opts[] = {
    { "hold", required_argument, NULL, 'H' },
    { NULL,   0,                 NULL, 0   },
};

int main(int argc, char *argv[])
{
    const char *host = "127.0.0.1";
    int port = DEFAULT_PORT;
    int key = CAESAR_DEFAULT_KEY;
    int hold = 0;
    int opt;

    log_init_from_env();

    /* 選項字串開頭的 '+'：遇到第一個「非選項」參數（指令）就停止解析。
     * GNU getopt 預設會「重排」參數，把後面的 -x 也當成選項，
     * 例如 `./client ECHO -5` 會把 -5 誤認成未知選項而失敗。
     * 加上 '+' 之後，指令後面的內容一律原樣當成指令的一部分。 */
    while ((opt = getopt_long(argc, argv, "+h:p:k:v:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'H':
            hold = parse_int(optarg, 3600);
            if (hold < 0) {
                fprintf(stderr, "invalid hold: %s (expected 0-3600 seconds)\n", optarg);
                return 1;
            }
            break;
        case 'k':
            key = parse_int(optarg, 94);
            if (key < 0) {
                fprintf(stderr, "invalid shift: %s (expected 0-94)\n", optarg);
                return 1;
            }
            break;
        case 'h':
            host = optarg;
            break;
        case 'p':
            port = parse_int(optarg, 65535);
            if (port <= 0) {
                fprintf(stderr, "invalid port: %s\n", optarg);
                return 1;
            }
            break;
        case 'v': {
            int lv = parse_int(optarg, LOG_DEBUG);
            if (lv < 0) {
                fprintf(stderr, "invalid log level: %s (expected 0-2)\n", optarg);
                return 1;
            }
            log_set_level(lv);
            break;
        }
        default:
            usage(argv[0]);
            return 1;
        }
    }
    if (optind >= argc) {
        usage(argv[0]);
        return 1;
    }

    /* 把剩下的參數用空白串起來。上限 MAX_LINE-2：留給 send_line 補的 '\n' 與 '\0'。
     * 用 snprintf 帶長度寫入，避免溢位；超過上限就直接拒絕。 */
    char line[MAX_LINE];
    size_t len = 0;
    for (int i = optind; i < argc; i++) {
        int w = snprintf(line + len, sizeof(line) - 1 - len, "%s%s",
                         (i > optind) ? " " : "", argv[i]);
        if (w < 0 || (size_t)w >= sizeof(line) - 1 - len) {
            LOG_E("input too long (max %d bytes)", MAX_LINE - 2);
            return 1;
        }
        len += (size_t)w;
    }

    /* 把文字 IP 轉成 32-bit 網路位元組序的位址 */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        LOG_E("invalid IPv4 address: %s", host);
        return 1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_E("socket: %s", strerror(errno));
        return 1;
    }

    /* connect：主動發起三向交握（SYN → SYN-ACK → ACK），完成後才回傳。
     * client 不需要 bind，核心會自動挑一個暫時埠（ephemeral port）。 */
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_E("connect %s:%d: %s", host, port, strerror(errno));
        close(fd);
        return 1;
    }
    LOG_D("connected to %s:%d (fd=%d)", host, port, fd);

    /* 加密「行的內容」，'\n' 由 send_line 在加密之後才補上。
     * 密文只會落在 32–126，不會產生 '\n'，訊息邊界不受影響。 */
    LOG_D("plain: %s", line);
    caesar_encrypt(line, key);

    /* send_line 會拒絕內含 '\n' 的字串（errno=EMSGSIZE），
     * 因為 '\n' 是本協定唯一的訊息邊界，內容裡出現就會被切成兩則訊息。 */
    if (send_line(fd, line) < 0) {
        if (errno == EMSGSIZE)
            LOG_E("input must be a single line shorter than %d bytes", MAX_LINE - 1);
        else
            LOG_E("send_line: %s", strerror(errno));
        close(fd);
        return 1;
    }
    LOG_D("sent cipher: %s", line);

    char reply[MAX_LINE];
    ssize_t n = recv_line(fd, reply, sizeof(reply));
    if (n == 0) {
        LOG_E("server closed connection without reply");
        close(fd);
        return 1;
    }
    if (n < 0) {
        LOG_E("recv_line: %s", strerror(errno));
        close(fd);
        return 1;
    }
    reply[n - 1] = '\0';              /* 先去掉 '\n'，只解密內容 */
    LOG_D("recv cipher: %s", reply);
    caesar_decrypt(reply, key);

    printf("%s\n", reply);

    if (hold > 0) {
        /* 先 flush：stdout 接到檔案或管線時是「全緩衝」，不 flush 的話，
         * 回覆要等到 hold 結束、程式結束時才會真正寫出去，壓測腳本就看不到即時結果。 */
        fflush(stdout);
        LOG_D("holding connection for %d s", hold);
        /* sleep 被信號打斷時會提早回傳「剩餘秒數」，用迴圈睡滿 */
        unsigned left = (unsigned)hold;
        while (left > 0)
            left = sleep(left);
    }

    /* close 會送出 FIN；server 的 recv 因此回傳 0，得知對方已結束 */
    if (close(fd) < 0)
        LOG_E("close: %s", strerror(errno));
    return 0;
}
