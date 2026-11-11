/*
 * 階段 2：最小 client，改用 libutil.so 的 send_line / recv_line 與 log
 *
 * 流程：socket → connect → 送出一行 → 讀回一行 → 印出 → close
 *
 * 用法：./client [-h host] [-p port] [-v level] <text...>
 *   命令列剩下的參數以空白串成一行送出（send_line 會補 '\n'）。
 *   例：./client -p 9000 hello world   → 送出 "hello world\n"
 *
 * 輸出分流：server 的回覆印到 stdout；log 一律寫到 stderr。
 * 這樣 `./client ... > out.txt` 只會存到回覆，不會混進 log。
 */
#include <arpa/inet.h>   /* inet_pton, htons */
#include <errno.h>
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
    fprintf(stderr, "usage: %s [-h host] [-p port] [-v level(0-2)] <text...>\n", prog);
}

int main(int argc, char *argv[])
{
    const char *host = "127.0.0.1";
    int port = DEFAULT_PORT;
    int opt;

    log_init_from_env();

    while ((opt = getopt(argc, argv, "h:p:v:")) != -1) {
        switch (opt) {
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
    LOG_D("sent: %s", line);

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
    LOG_D("received %zd bytes", n);

    /* 結尾已有 '\n'，原樣印出即可 */
    fputs(reply, stdout);

    /* close 會送出 FIN；server 的 recv 因此回傳 0，得知對方已結束 */
    if (close(fd) < 0)
        LOG_E("close: %s", strerror(errno));
    return 0;
}
