/*
 * 階段 3：併發 echo server（每個 client 一個子行程）
 *
 * 流程：socket → setsockopt(SO_REUSEADDR) → bind → listen → 迴圈 accept → fork
 *   子行程：close(listenfd) → 反覆「讀一行、回同一行」→ close(connfd) → exit(0)
 *   父行程：close(connfd) → 立刻回到 accept 等下一個 client
 *
 * 注意：此階段「刻意不處理 SIGCHLD」。子行程結束後父行程不會 wait，
 * 會留下殭屍行程（ps 顯示 Z / <defunct>）。階段 6 的 R1 再用 robust 版修正並對照。
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
#include <sys/types.h>   /* pid_t */
#include <unistd.h>      /* close, getopt, fork, getpid */

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
     *    listenfd 繼續負責接新連線，connfd 專門跟這個 client 對話。
     *    父行程只負責 accept + fork，真正的對話交給子行程，所以不會被單一 client 卡住。 */
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

        /* 6. fork：複製出一個幾乎一模一樣的子行程，包括「fd 表」。
         *    fork 之後，listenfd 與 connfd 在父、子兩邊「都」是開著的，
         *    兩邊的 fd 指向核心中同一個 socket 物件（物件內有參考計數）。 */
        pid_t pid = fork();
        if (pid < 0) {
            /* fork 失敗（例如行程數達上限）：這個 client 無法服務，關掉連線，server 繼續運作 */
            LOG_E("fork for %s: %s", peer, strerror(errno));
            close(connfd);
            continue;
        }

        if (pid == 0) {
            /* ===== 子行程 ===== */
            /* 子行程不負責接新連線，關掉自己那份 listenfd。
             * 不關的話：每個子行程都握著監聽 socket，父行程結束後 port 仍被佔住，
             * 而且浪費 fd。 */
            if (close(listenfd) < 0)
                LOG_E("child close(listenfd): %s", strerror(errno));

            LOG_I("%s connected (child pid=%d, connfd=%d)", peer, (int)getpid(), connfd);
            handle_client(connfd, peer);
            LOG_I("%s disconnected", peer);

            if (close(connfd) < 0)
                LOG_E("close(%d): %s", connfd, strerror(errno));
            /* 子行程一定要在這裡結束，否則會掉回 for 迴圈去 accept，
             * 變成「子行程也在當 server」。 */
            exit(0);
        }

        /* ===== 父行程 ===== */
        /* 父行程不跟這個 client 對話，關掉自己那份 connfd。
         * 這「非常重要」：socket 的參考計數要降到 0，核心才會真的送出 FIN。
         * 父行程若不關，子行程 close 之後連線仍不會關閉（client 收不到 EOF），
         * 而且每個連線洩漏一個 fd，很快就會用完（EMFILE）。 */
        LOG_D("forked child pid=%d for %s", (int)pid, peer);
        if (close(connfd) < 0)
            LOG_E("parent close(connfd=%d): %s", connfd, strerror(errno));
    }

    /* 不會到這裡；階段 6 之後會用 SIGINT 處理優雅結束 */
    close(listenfd);
    return 0;
}
