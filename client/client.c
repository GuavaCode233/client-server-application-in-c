/*
 * 階段 1：最小 client
 *
 * 流程：socket → connect → 送出一行 → 讀回一行 → 印出 → close
 *
 * 用法：./client [-h host] [-p port] <text...>
 *   命令列剩下的參數以空白串成一行，結尾補 '\n' 後送出。
 *   例：./client -p 9000 hello world   → 送出 "hello world\n"
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

#define MAX_LINE 1024

/* 與 server 相同的「一次讀 1 byte 直到 '\n'」；階段 2 會統一移進 libutil.so */
static ssize_t read_line_simple(int fd, char *buf, size_t maxlen)
{
    size_t n = 0;
    while (n < maxlen - 1) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r == 0)
            return (n == 0) ? 0 : -1;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "[client] recv: %s\n", strerror(errno));
            return -1;
        }
        buf[n++] = c;
        if (c == '\n')
            break;
    }
    buf[n] = '\0';
    if (n > 0 && buf[n - 1] != '\n') {
        fprintf(stderr, "[client] reply too long\n");
        return -1;
    }
    return (ssize_t)n;
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-h host] [-p port] <text...>\n", prog);
}

int main(int argc, char *argv[])
{
    const char *host = "127.0.0.1";
    int port = DEFAULT_PORT;
    int opt;

    while ((opt = getopt(argc, argv, "h:p:")) != -1) {
        switch (opt) {
        case 'h':
            host = optarg;
            break;
        case 'p':
            port = atoi(optarg);
            if (port <= 0 || port > 65535) {
                fprintf(stderr, "invalid port: %s\n", optarg);
                return 1;
            }
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }
    if (optind >= argc) {
        usage(argv[0]);
        return 1;
    }

    /* 把剩下的參數用空白串起來，留 2 byte 給 '\n' 與 '\0'。
     * 用 snprintf 帶長度寫入，避免溢位；超過上限就直接拒絕。 */
    char line[MAX_LINE];
    size_t len = 0;
    for (int i = optind; i < argc; i++) {
        int w = snprintf(line + len, sizeof(line) - 1 - len, "%s%s",
                         (i > optind) ? " " : "", argv[i]);
        if (w < 0 || (size_t)w >= sizeof(line) - 1 - len) {
            fprintf(stderr, "[client] input too long (max %d bytes)\n", MAX_LINE - 2);
            return 1;
        }
        len += (size_t)w;
    }
    /* '\n' 是本協定唯一的訊息邊界；內容本身不能含 '\n'，否則會被當成兩行 */
    if (memchr(line, '\n', len) != NULL) {
        fprintf(stderr, "[client] input must not contain newline\n");
        return 1;
    }
    line[len++] = '\n';
    line[len] = '\0';

    /* 把文字 IP 轉成 32-bit 網路位元組序的位址 */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "[client] invalid IPv4 address: %s\n", host);
        return 1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[client] socket: %s\n", strerror(errno));
        return 1;
    }

    /* connect：主動發起三向交握（SYN → SYN-ACK → ACK），完成後才回傳。
     * client 不需要 bind，核心會自動挑一個暫時埠（ephemeral port）。 */
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[client] connect %s:%d: %s\n", host, port, strerror(errno));
        close(fd);
        return 1;
    }

    /* 階段 1 簡化：一次 send；階段 2 改用 send_all 處理部分寫入 */
    ssize_t s = send(fd, line, len, 0);
    if (s < 0) {
        fprintf(stderr, "[client] send: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    if ((size_t)s != len) {
        fprintf(stderr, "[client] short write: %zd/%zu bytes\n", s, len);
        close(fd);
        return 1;
    }

    char reply[MAX_LINE];
    ssize_t n = read_line_simple(fd, reply, sizeof(reply));
    if (n <= 0) {
        fprintf(stderr, "[client] %s\n", n == 0 ? "server closed connection" : "failed to read reply");
        close(fd);
        return 1;
    }

    /* 輸出時去掉結尾 '\n'，再由 printf 補上，讓格式一致 */
    printf("%.*s\n", (int)(n - 1), reply);

    /* close 會送出 FIN；server 的 recv 因此回傳 0，得知對方已結束 */
    close(fd);
    return 0;
}
