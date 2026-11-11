/*
 * 階段 2：迭代式 echo server，改用 libutil.so 的 recv_line / send_line 與 log
 *
 * 流程：socket → setsockopt(SO_REUSEADDR) → bind → listen → 迴圈 accept
 *       → 對該 client 反覆「讀一行、回同一行」→ client 關閉後 close，回到 accept
 *
 * 仍是「迭代式」server：一次只服務一個 client。階段 3 才用 fork() 解決。
 *
 * Log 等級：
 *   執行期：環境變數 LOG_LEVEL=0|1|2，或 -v level（-v 優先）
 *   編譯期：make DEBUG=1 才會把 LOG_D 編進執行檔
 */
#include <arpa/inet.h>   /* inet_ntop, htons, ntohs */
#include <errno.h>
#include <netinet/in.h>  /* sockaddr_in, INADDR_ANY */
#include <stdio.h>
#include <stdlib.h>      /* strtol */
#include <string.h>      /* strerror, memset */
#include <sys/socket.h>
#include <unistd.h>      /* close, getopt */

#include "util.h"

#ifndef DEFAULT_PORT
#define DEFAULT_PORT 9000   /* Makefile 會用 -DDEFAULT_PORT 覆寫 */
#endif

#define BACKLOG 16          /* listen 佇列長度，見 CONTEXT.md B8 */

/* 解析 0~max 的整數參數；格式錯誤回傳 -1 */
static int parse_int(const char *s, int max)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (s[0] == '\0' || *end != '\0' || v < 0 || v > max)
        return -1;
    return (int)v;
}

/* 服務單一 client：讀一行，原樣送回，直到對方關閉。 */
static void handle_client(int connfd, const char *peer)
{
    char line[MAX_LINE];
    ssize_t n;

    for (;;) {
        n = recv_line(connfd, line, sizeof(line));
        if (n == 0) {
            LOG_D("%s sent FIN", peer);
            return;
        }
        if (n < 0) {
            if (errno == EMSGSIZE)
                LOG_E("%s line too long (> %d bytes), closing", peer, MAX_LINE - 1);
            else
                LOG_E("%s recv_line: %s", peer, strerror(errno));
            return;
        }

        line[n - 1] = '\0';   /* 去掉 '\n'：協定層的邊界，不是內容的一部分 */
        LOG_I("%s recv: %s", peer, line);
        LOG_D("%s recv %zd bytes (incl. newline)", peer, n);

        if (send_line(connfd, line) < 0) {
            LOG_E("%s send_line: %s", peer, strerror(errno));
            return;
        }
        LOG_D("%s sent reply", peer);
    }
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-p port] [-v level(0-2)]\n", prog);
}

int main(int argc, char *argv[])
{
    int port = DEFAULT_PORT;
    int opt;

    /* 先讀環境變數，再讓命令列 -v 覆寫：越「靠近這次執行」的設定優先 */
    log_init_from_env();

    while ((opt = getopt(argc, argv, "p:v:")) != -1) {
        switch (opt) {
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

#ifdef ROBUST
    const char *variant = "robust";
#else
    const char *variant = "naive";
#endif

    /* 1. socket：向核心要一個 TCP 端點，拿回一個檔案描述符（fd） */
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        LOG_E("socket: %s", strerror(errno));
        return 1;
    }

    /* 2. SO_REUSEADDR：server 關掉後，舊連線會在 TIME_WAIT 停留一段時間，
     *    沒設這個選項，立刻重啟會 bind 失敗（Address already in use）。 */
    int on = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        LOG_E("setsockopt(SO_REUSEADDR): %s", strerror(errno));
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
        LOG_E("bind port %d: %s", port, strerror(errno));
        close(listenfd);
        return 1;
    }

    /* 4. listen：把 socket 轉成「被動」模式，核心開始替我們完成三向交握，
     *    完成的連線先放在佇列（最多 BACKLOG 個）等 accept 取走。 */
    if (listen(listenfd, BACKLOG) < 0) {
        LOG_E("listen: %s", strerror(errno));
        close(listenfd);
        return 1;
    }

    LOG_I("server (%s, %s) listening on port %d", variant, util_version(), port);
    LOG_D("debug log compiled in and enabled");

    /* 5. accept 迴圈：每次取出一條已建立的連線，得到一個「新的」fd（connfd）。
     *    listenfd 繼續負責接新連線，connfd 專門跟這個 client 對話。 */
    for (;;) {
        struct sockaddr_in cli;
        socklen_t clilen = sizeof(cli);
        int connfd = accept(listenfd, (struct sockaddr *)&cli, &clilen);
        if (connfd < 0) {
            if (errno == EINTR)
                continue;
            LOG_E("accept: %s", strerror(errno));
            continue;   /* 單一連線失敗不該讓整個 server 結束 */
        }

        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip)) == NULL)
            snprintf(ip, sizeof(ip), "?");
        char peer[INET_ADDRSTRLEN + 8];
        snprintf(peer, sizeof(peer), "%s:%u", ip, (unsigned)ntohs(cli.sin_port));

        LOG_I("%s connected (connfd=%d)", peer, connfd);
        handle_client(connfd, peer);
        LOG_I("%s disconnected", peer);

        if (close(connfd) < 0)   /* 不 close 會造成 fd 洩漏，對方也收不到 FIN */
            LOG_E("close(%d): %s", connfd, strerror(errno));
    }

    /* 不會到這裡；階段 6 之後會用 SIGINT 處理優雅結束 */
    close(listenfd);
    return 0;
}
