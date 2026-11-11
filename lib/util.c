#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

const char *util_version(void)
{
    return "libutil 0.2 (stage 2)";
}

/* ======================================================================
 *  網路 I/O
 * ====================================================================== */

ssize_t send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t left = len;

    /* send 只保證「送出 ≥ 1 byte」，不保證全部送出：
     * 核心的傳送緩衝區若剩餘空間不足，就只收下放得進去的部分並回傳實際數量。
     * 所以必須迴圈，直到剩餘量為 0。 */
    while (left > 0) {
        ssize_t n = send(fd, p, left, 0);
        if (n < 0) {
            if (errno == EINTR)   /* 還沒送出任何資料就被信號打斷，重送即可 */
                continue;
            return -1;
        }
        p    += n;
        left -= (size_t)n;
    }
    return (ssize_t)len;
}

/* 從 socket 「真的」取走恰好 len bytes（這些資料已確認在接收緩衝區中）。 */
static int consume_exact(int fd, char *dst, size_t len)
{
    while (len > 0) {
        ssize_t n = recv(fd, dst, len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {             /* 理論上不會發生：資料剛剛 peek 過還在 */
            errno = ECONNRESET;
            return -1;
        }
        dst += n;
        len -= (size_t)n;
    }
    return 0;
}

ssize_t recv_line(int fd, char *buf, size_t maxlen)
{
    if (buf == NULL || maxlen < 2) {   /* 至少要放得下 '\n' 與 '\0' */
        errno = EINVAL;
        return -1;
    }

    size_t have = 0;
    size_t cap  = maxlen - 1;          /* 保留 1 byte 給 '\0' */

    /*
     * 為什麼用 MSG_PEEK？
     * 一次 recv 一大塊比一次 1 byte 快很多，但可能多讀到「下一行」的開頭。
     * 多讀的部分若存在函式內的 static 緩衝區，就變成隱藏狀態（換 fd 就錯）。
     * MSG_PEEK 讓我們「先偷看」核心緩衝區的資料而不取走：
     *   1. peek 一大塊，找 '\n'
     *   2. 只取走到 '\n' 為止的部分；之後的資料留在核心裡，下次再讀
     * 函式因此不需要保存任何狀態，每行約 2 次系統呼叫。
     */
    while (have < cap) {
        ssize_t n = recv(fd, buf + have, cap - have, MSG_PEEK);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {                  /* 對方送了 FIN */
            if (have == 0)
                return 0;              /* 乾淨的結束 */
            errno = ECONNRESET;        /* 一行讀到一半就斷線 */
            return -1;
        }

        char *nl = memchr(buf + have, '\n', (size_t)n);
        size_t take = nl ? (size_t)(nl - (buf + have)) + 1 : (size_t)n;

        if (consume_exact(fd, buf + have, take) < 0)
            return -1;
        have += take;

        if (nl) {
            buf[have] = '\0';
            return (ssize_t)have;
        }
    }

    /* 緩衝區滿了仍沒有 '\n' → 超長。剩下的資料還在 socket 裡，呼叫端應斷線。 */
    buf[have] = '\0';
    errno = EMSGSIZE;
    return -1;
}

int send_line(int fd, const char *line)
{
    size_t len = strlen(line);

    /* 內容含 '\n' 會被對方當成兩行；加上 '\n' 後超過 MAX_LINE 對方也會拒收 */
    if (len + 1 > MAX_LINE || memchr(line, '\n', len) != NULL) {
        errno = EMSGSIZE;
        return -1;
    }

    /* 先組成一整塊再送：一次 send_all 送出「內容 + '\n'」，
     * 而不是送兩次（那會多一次系統呼叫，也可能拆成兩個 TCP 封包）。 */
    char tmp[MAX_LINE];
    memcpy(tmp, line, len);
    tmp[len] = '\n';

    return (send_all(fd, tmp, len + 1) < 0) ? -1 : 0;
}

/* ======================================================================
 *  Log
 * ====================================================================== */

/* 每個行程一份（fork 後子行程拿到的是複本），所以不需要跨行程同步 */
static int g_log_level = LOG_INFO;

void log_set_level(int level)
{
    if (level < LOG_ERROR) level = LOG_ERROR;
    if (level > LOG_DEBUG) level = LOG_DEBUG;
    g_log_level = level;
}

void log_init_from_env(void)
{
    const char *s = getenv("LOG_LEVEL");
    if (s == NULL || s[0] == '\0')
        return;

    /* 只接受單一個數字 0~2；用 strtol 而非 atoi，才能偵測格式錯誤 */
    char *end;
    long v = strtol(s, &end, 10);
    if (*end != '\0' || v < LOG_ERROR || v > LOG_DEBUG) {
        fprintf(stderr, "LOG_LEVEL=%s ignored (expected 0, 1 or 2)\n", s);
        return;
    }
    g_log_level = (int)v;
}

void log_write(int level, const char *file, int line, const char *fmt, ...)
{
    if (level < LOG_ERROR)
        level = LOG_ERROR;
    if (level > g_log_level)
        return;

    /* 先保存 errno：呼叫端常寫 LOG_E("...%s", strerror(errno))，
     * 而下面的 clock_gettime / write 可能改動 errno，避免影響呼叫端後續判斷。 */
    int saved_errno = errno;

    static const char *const names[] = { "ERROR", "INFO ", "DEBUG" };

    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);       /* _r 版本：不使用共用的 static 結構 */

    /* body 最後留 1 byte 給結尾 '\n'；snprintf 系列寫的 '\0' 不會被送出 */
    char msg[MAX_LINE + 256];
    const size_t body = sizeof(msg) - 1;

    int w = snprintf(msg, body,
                     "[%04d-%02d-%02d %02d:%02d:%02d.%03ld][%s][%d][%s:%d] ",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L,
                     names[level], (int)getpid(), file, line);
    /* snprintf 回傳「想寫的長度」，可能大於實際寫入；截斷時實際只有 body-1 個字元 */
    size_t len = (w < 0) ? 0 : ((size_t)w < body ? (size_t)w : body - 1);

    va_list ap;
    va_start(ap, fmt);
    w = vsnprintf(msg + len, body - len, fmt, ap);
    va_end(ap);
    if (w > 0)
        len += ((size_t)w < body - len) ? (size_t)w : body - len - 1;

    msg[len++] = '\n';                  /* len ≤ body，所以 '\n' 一定放得下 */

    /* 為什麼用 write(2) 一次寫出整行，而不是 fprintf(stderr, ...)？
     * 階段 3 之後會有很多子行程同時寫 log 到同一個 stderr。
     * 一次 write 寫出完整的一行，各行程的 log 才不會互相穿插成亂碼。 */
    ssize_t r;
    do {
        r = write(STDERR_FILENO, msg, len);
    } while (r < 0 && errno == EINTR);

    errno = saved_errno;
}
