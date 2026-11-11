/*
 * 階段 1：最小 echo server（無 fork、無加密）
 *
 * 流程：socket → setsockopt(SO_REUSEADDR) → bind → listen → 迴圈 accept
 *       → 對該 client 反覆「讀一行、回同一行」→ client 關閉後 close，回到 accept
 *
 * 這是「迭代式（iterative）server」：一次只服務一個 client，
 * 第二個 client 會卡在 listen 佇列裡，直到第一個斷線。階段 3 才用 fork() 解決。
 */
#include <arpa/inet.h>   /* inet_ntop, htons, ntohs */
#include <errno.h>
#include <netinet/in.h>  /* sockaddr_in, INADDR_ANY */
#include <stdio.h>
#include <stdlib.h>      /* atoi, exit */
#include <string.h>      /* strerror, memset */
#include <sys/socket.h>
#include <unistd.h>      /* close, getopt */

#include "util.h"

#ifndef DEFAULT_PORT
#define DEFAULT_PORT 9000   /* Makefile 會用 -DDEFAULT_PORT 覆寫 */
#endif

#define MAX_LINE 1024       /* 單行上限（含 '\n'），見 CONTEXT.md B4 */
#define BACKLOG  16         /* listen 佇列長度，見 B8 */

/*
 * 讀一行（到 '\n' 為止）。
 *
 * 為什麼一次只讀 1 個 byte？
 * TCP 是「位元組串流」，沒有訊息邊界：client 送兩行，server 可能一次 recv 就全部收到，
 * 也可能一行被拆成好幾次收到。一次讀 1 byte 最直觀，保證不會多讀到下一行的內容。
 * 缺點是每個 byte 都要一次系統呼叫，效率差；階段 2 會換成 libutil 的 recv_line（帶緩衝）。
 *
 * 回傳：行長度（含 '\n'）；0 = 對方已關閉；-1 = 錯誤或超過上限。
 */
static ssize_t read_line_simple(int fd, char *buf, size_t maxlen)
{
    size_t n = 0;
    while (n < maxlen - 1) {             /* 保留 1 byte 給結尾的 '\0' */
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r == 0)                      /* 對方正常關閉（送了 FIN），不是錯誤 */
            return (n == 0) ? 0 : -1;    /* 行讀到一半就斷線，視為不完整 */
        if (r < 0) {
            if (errno == EINTR)          /* 被信號中斷，重試即可 */
                continue;
            fprintf(stderr, "[server] recv: %s\n", strerror(errno));
            return -1;
        }
        buf[n++] = c;
        if (c == '\n')
            break;
    }
    buf[n] = '\0';
    if (n > 0 && buf[n - 1] != '\n') {   /* 塞滿了還沒看到 '\n' → 超長 */
        fprintf(stderr, "[server] line too long (> %d bytes)\n", MAX_LINE - 1);
        return -1;
    }
    return (ssize_t)n;
}

/* 服務單一 client：讀一行，原樣送回，直到對方關閉。 */
static void handle_client(int connfd, const char *peer)
{
    char line[MAX_LINE];
    ssize_t n;

    while ((n = read_line_simple(connfd, line, sizeof(line))) > 0) {
        /* 印 log 時去掉 '\n'，只是為了顯示好看；送回的內容維持原樣 */
        printf("[server] %s recv: %.*s\n", peer, (int)(n - 1), line);

        /* 階段 1 簡化：假設一次 send 就送完。
         * 實際上 send 可能只送出一部分（short write），階段 2 用 send_all 處理。 */
        ssize_t s = send(connfd, line, (size_t)n, 0);
        if (s < 0) {
            fprintf(stderr, "[server] send: %s\n", strerror(errno));
            return;
        }
        if (s != n) {
            fprintf(stderr, "[server] short write: %zd/%zd bytes\n", s, n);
            return;
        }
    }
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-p port]\n", prog);
}

int main(int argc, char *argv[])
{
    int port = DEFAULT_PORT;
    int opt;

    while ((opt = getopt(argc, argv, "p:")) != -1) {
        switch (opt) {
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

#ifdef ROBUST
    const char *variant = "robust";
#else
    const char *variant = "naive";
#endif

    /* 1. socket：向核心要一個 TCP 端點，拿回一個檔案描述符（fd） */
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        fprintf(stderr, "[server] socket: %s\n", strerror(errno));
        return 1;
    }

    /* 2. SO_REUSEADDR：server 關掉後，舊連線會在 TIME_WAIT 停留一段時間，
     *    沒設這個選項，立刻重啟會 bind 失敗（Address already in use）。 */
    int on = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        fprintf(stderr, "[server] setsockopt(SO_REUSEADDR): %s\n", strerror(errno));
        close(listenfd);
        return 1;
    }

    /* 3. bind：把這個 socket 綁到「本機任一 IP : port」。
     *    port 與 IP 必須轉成網路位元組序（big-endian）：htons / htonl。 */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[server] bind port %d: %s\n", port, strerror(errno));
        close(listenfd);
        return 1;
    }

    /* 4. listen：把 socket 轉成「被動」模式，核心開始替我們完成三向交握，
     *    完成的連線先放在佇列（最多 BACKLOG 個）等 accept 取走。 */
    if (listen(listenfd, BACKLOG) < 0) {
        fprintf(stderr, "[server] listen: %s\n", strerror(errno));
        close(listenfd);
        return 1;
    }

    printf("[server] (%s, %s) listening on port %d\n", variant, util_version(), port);
    fflush(stdout);

    /* 5. accept 迴圈：每次取出一條已建立的連線，得到一個「新的」fd（connfd）。
     *    listenfd 繼續負責接新連線，connfd 專門跟這個 client 對話。 */
    for (;;) {
        struct sockaddr_in cli;
        socklen_t clilen = sizeof(cli);
        int connfd = accept(listenfd, (struct sockaddr *)&cli, &clilen);
        if (connfd < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "[server] accept: %s\n", strerror(errno));
            continue;   /* 單一連線失敗不該讓整個 server 結束 */
        }

        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip)) == NULL)
            snprintf(ip, sizeof(ip), "?");
        char peer[INET_ADDRSTRLEN + 8];
        snprintf(peer, sizeof(peer), "%s:%u", ip, (unsigned)ntohs(cli.sin_port));

        printf("[server] %s connected\n", peer);
        fflush(stdout);

        handle_client(connfd, peer);

        printf("[server] %s disconnected\n", peer);
        fflush(stdout);
        close(connfd);   /* 不 close 會造成 fd 洩漏，對方也收不到 FIN */
    }

    /* 不會到這裡；階段 6 之後會用 SIGINT 處理優雅結束 */
    close(listenfd);
    return 0;
}
