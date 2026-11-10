/*
 * server.c - minimal static file server in pure C17 (POSIX).
 *
 *   GET / HEAD only, one request per connection (Connection: close)
 *   /healthz          -> 200 "ok" (for Render health checks)
 *   security headers  -> on every response, errors included
 *   path safety       -> percent-decoding, rejects "..", NUL and control
 *                        bytes, hides dotfiles, and every file is resolved
 *                        with realpath() and must stay inside the root
 *
 * Configuration (environment):
 *   PORT  port to listen on            (default 8080; Render sets it)
 *   HOST  IPv4 or IPv6 address to bind (default: dual-stack "::", which
 *         also accepts IPv4; falls back to 0.0.0.0 without IPv6)
 *   ROOT  directory to serve           (default "public"; argv[1] wins)
 *   CANONICAL_HOST  optional, e.g. "unemployed.lt.eu.org": every request
 *         whose Host differs (such as *.onrender.com) gets a 301 to
 *         https://CANONICAL_HOST with the same path and query. /healthz is
 *         exempt and always answers 200. Unset or empty: Host is ignored.
 */
/* XSI (SUSv4) = POSIX.1-2008 + realpath, SA_RESTART. glibc exposes these with
 * _POSIX_C_SOURCE alone, musl (Alpine) needs _XOPEN_SOURCE under -std=c17.
 * _XOPEN_SOURCE 700 implies _POSIX_C_SOURCE 200809L, so only one is defined. */
#define _XOPEN_SOURCE 700

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define REQ_MAX       8192   /* request line + headers */
#define TARGET_MAX    2048
#define IO_TIMEOUT_S  10
#define MAX_CHILDREN  64

static const char SECURITY_HEADERS[] =
    "X-Content-Type-Options: nosniff\r\n"
    "X-Frame-Options: DENY\r\n"
    "Referrer-Policy: strict-origin-when-cross-origin\r\n"
    "Permissions-Policy: camera=(), microphone=(), geolocation=(), payment=()\r\n"
    "Cross-Origin-Opener-Policy: same-origin\r\n"
    "Strict-Transport-Security: max-age=31536000\r\n"
    /* The page keeps CSS and JS inline (one file) and loads Google Fonts. */
    "Content-Security-Policy: default-src 'self'; "
    "script-src 'self' 'unsafe-inline'; "
    "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; "
    "font-src https://fonts.gstatic.com; img-src 'self' data:; "
    "connect-src 'self'; object-src 'none'; base-uri 'none'; "
    "form-action 'self'; frame-ancestors 'none'\r\n";

static char root_dir[PATH_MAX];
static size_t root_len;
static char canonical[256]; /* lower-case, no trailing dot; "" = off */
static volatile sig_atomic_t children;

/* ------------------------------------------------------------------ */

static void on_sigchld(int sig)
{
    (void)sig;
    int saved = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        if (children > 0) children--;
    }
    errno = saved;
}

static bool write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        buf += n;
        len -= (size_t)n;
    }
    return true;
}

static void http_date(char *out, size_t cap, time_t t)
{
    struct tm tm;
    if (!gmtime_r(&t, &tm) || strftime(out, cap, "%a, %d %b %Y %H:%M:%S GMT", &tm) == 0)
        snprintf(out, cap, "Thu, 01 Jan 1970 00:00:00 GMT");
}

static const char *reason_for(int status)
{
    switch (status) {
    case 200: return "OK";
    case 301: return "Moved Permanently";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default:  return "Error";
    }
}

/* Sends the status line and headers. `extra` is zero or more full header lines. */
static bool send_head(int fd, int status, const char *ctype, long long length,
                      const char *cache, const char *extra)
{
    char date[64], head[3072];
    http_date(date, sizeof date, time(NULL));
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\n"
                     "Date: %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lld\r\n"
                     "Cache-Control: %s\r\n"
                     "Connection: close\r\n"
                     "%s%s\r\n",
                     status, reason_for(status), date, ctype, length, cache,
                     SECURITY_HEADERS, extra ? extra : "");
    if (n < 0 || (size_t)n >= sizeof head) return false;
    return write_all(fd, head, (size_t)n);
}

static int send_error(int fd, int status, bool head_only, const char *extra)
{
    char body[96];
    int n = snprintf(body, sizeof body, "%d %s\n", status, reason_for(status));
    if (n < 0) n = 0;
    if (send_head(fd, status, "text/plain; charset=utf-8", n, "no-store", extra) && !head_only)
        write_all(fd, body, (size_t)n);
    return status;
}

static const char *content_type(const char *path)
{
    static const struct { const char *ext, *type; } types[] = {
        {"html", "text/html; charset=utf-8"},  {"htm", "text/html; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},    {"js", "text/javascript; charset=utf-8"},
        {"mjs", "text/javascript; charset=utf-8"},
        {"json", "application/json"},          {"map", "application/json"},
        {"webmanifest", "application/manifest+json"},
        {"txt", "text/plain; charset=utf-8"},  {"xml", "application/xml"},
        {"svg", "image/svg+xml"},              {"png", "image/png"},
        {"jpg", "image/jpeg"},                 {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},                  {"webp", "image/webp"},
        {"avif", "image/avif"},                {"ico", "image/x-icon"},
        {"woff", "font/woff"},                 {"woff2", "font/woff2"},
        {"pdf", "application/pdf"},
    };
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(slash ? slash : path, '.');
    if (dot) {
        for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
            if (strcasecmp(dot + 1, types[i].ext) == 0) return types[i].type;
    }
    return "application/octet-stream";
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decodes src into dst. Rejects bad escapes, NUL and control bytes. */
static bool url_decode(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    for (const char *p = src; *p; p++) {
        int c = (unsigned char)*p;
        if (c == '%') {
            int hi = hexval((unsigned char)p[1]), lo = hi < 0 ? -1 : hexval((unsigned char)p[2]);
            if (hi < 0 || lo < 0) return false;
            c = hi * 16 + lo;
            p += 2;
        }
        if (c < 0x20 || c == 0x7f) return false;
        if (o + 1 >= cap) return false;
        dst[o++] = (char)c;
    }
    dst[o] = '\0';
    return true;
}

/* 0 = fine, 400 = "." or ".." segment, 404 = dotfile segment. */
static int check_segments(const char *path)
{
    const char *seg = path;
    while (*seg) {
        while (*seg == '/') seg++;
        if (!*seg) break;
        size_t len = strcspn(seg, "/");
        if ((len == 1 && seg[0] == '.') || (len == 2 && seg[0] == '.' && seg[1] == '.'))
            return 400;
        if (seg[0] == '.') return 404;
        seg += len;
    }
    return 0;
}

static bool inside_root(const char *resolved)
{
    if (root_len == 1) return true; /* root is "/" */
    return strncmp(resolved, root_dir, root_len) == 0 &&
           (resolved[root_len] == '/' || resolved[root_len] == '\0');
}

/* Validates and normalises CANONICAL_HOST: a DNS name ([A-Za-z0-9.-]) with an
 * optional ":port". Returns false if it is not one. */
static bool set_canonical(const char *v)
{
    size_t n = strlen(v);
    if (n == 0) { canonical[0] = '\0'; return true; }
    if (n >= sizeof canonical || v[0] == '.' || v[0] == '-' || v[0] == ':') return false;
    bool in_port = false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)v[i];
        if (c == ':' && !in_port && i + 1 < n) { in_port = true; }
        else if (in_port ? (c >= '0' && c <= '9')
                         : ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '.' || c == '-')) {}
        else return false;
        canonical[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    canonical[n] = '\0';
    if (!in_port && canonical[n - 1] == '.') canonical[n - 1] = '\0';
    return canonical[0] != '\0';
}

/* Copies the Host header value into out. Returns how many Host headers the
 * request had: more than one is a bad request (RFC 9112, 3.2). */
static int find_host(const char *p, char *out, size_t cap)
{
    int found = 0;
    out[0] = '\0';
    while (*p && !(p[0] == '\r' && p[1] == '\n')) {
        const char *end = strstr(p, "\r\n");
        if (!end) break;
        if (end - p >= 5 && strncasecmp(p, "Host:", 5) == 0) {
            const char *v = p + 5, *e = end;
            while (v < e && (*v == ' ' || *v == '\t')) v++;
            while (e > v && (e[-1] == ' ' || e[-1] == '\t')) e--;
            size_t n = (size_t)(e - v);
            if (n >= cap) n = cap - 1; /* too long to be ours; still compared */
            memcpy(out, v, n);
            out[n] = '\0';
            found++;
        }
        p = end + 2;
    }
    return found;
}

/* Case-insensitive match against CANONICAL_HOST. If the canonical name has no
 * port, the Host port and one trailing dot are ignored. */
static bool host_is_canonical(char *host)
{
    if (!strchr(canonical, ':')) {
        char *colon = strrchr(host, ':');
        if (colon && host[0] != '[') {
            bool digits = colon[1] != '\0';
            for (const char *d = colon + 1; *d; d++) if (*d < '0' || *d > '9') digits = false;
            if (digits) *colon = '\0';
        }
        size_t n = strlen(host);
        if (n > 0 && host[n - 1] == '.') host[n - 1] = '\0';
    }
    return strcasecmp(host, canonical) == 0;
}

/* Reads until the end of the headers. 0 = ok, 431 = too large, -1 = gone. */
static int read_request(int fd, char *buf, size_t cap, size_t *out_len)
{
    size_t len = 0;
    for (;;) {
        if (len + 1 >= cap) return 431;
        ssize_t n = read(fd, buf + len, cap - 1 - len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        size_t from = len >= 3 ? len - 3 : 0;
        len += (size_t)n;
        buf[len] = '\0';
        if (strstr(buf + from, "\r\n\r\n")) {
            *out_len = len;
            return 0;
        }
    }
}

static int serve_file(int fd, const char *resolved, bool head_only, long long *sent)
{
    int file = open(resolved, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (file < 0) return send_error(fd, errno == EACCES ? 403 : 404, head_only, NULL);

    struct stat st;
    if (fstat(file, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(file);
        return send_error(fd, 404, head_only, NULL);
    }

    const char *ctype = content_type(resolved);
    const char *cache = strncmp(ctype, "text/html", 9) == 0 ? "no-cache" : "public, max-age=3600";
    char extra[96], mod[64];
    http_date(mod, sizeof mod, st.st_mtime);
    snprintf(extra, sizeof extra, "Last-Modified: %s\r\n", mod);

    if (!send_head(fd, 200, ctype, (long long)st.st_size, cache, extra) || head_only) {
        close(file);
        return 200;
    }
    char chunk[65536];
    for (;;) {
        ssize_t n = read(file, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (!write_all(fd, chunk, (size_t)n)) break;
        *sent += n;
    }
    close(file);
    return 200;
}

static int handle(int fd, char *method_out, size_t mcap, char *target_out, size_t tcap,
                  long long *sent)
{
    char req[REQ_MAX];
    size_t len = 0;
    int r = read_request(fd, req, sizeof req, &len);
    if (r == 431) return send_error(fd, 431, false, NULL);
    if (r != 0) return 0;

    /* Request line: METHOD SP TARGET SP VERSION CRLF */
    char *eol = strstr(req, "\r\n");
    *eol = '\0';
    const char *headers = eol + 2;
    char *method = req;
    char *sp1 = strchr(method, ' ');
    char *target = sp1 ? sp1 + 1 : NULL;
    char *sp2 = target ? strchr(target, ' ') : NULL;
    if (!sp1 || !sp2 || strchr(sp2 + 1, ' ')) return send_error(fd, 400, false, NULL);
    *sp1 = '\0';
    *sp2 = '\0';
    const char *version = sp2 + 1;

    for (const char *p = target; *p; p++)
        if ((unsigned char)*p <= 0x20 || (unsigned char)*p >= 0x7f || *p == '\\')
            return send_error(fd, 400, false, NULL);
    snprintf(method_out, mcap, "%.15s", method);
    snprintf(target_out, tcap, "%.200s", target);

    bool head_only = strcmp(method, "HEAD") == 0;
    if (strcmp(version, "HTTP/1.0") != 0 && strcmp(version, "HTTP/1.1") != 0)
        return send_error(fd, strncmp(version, "HTTP/", 5) == 0 ? 505 : 400, head_only, NULL);
    if (!head_only && strcmp(method, "GET") != 0)
        return send_error(fd, 405, false, "Allow: GET, HEAD\r\n");
    if (target[0] != '/' || strlen(target) > TARGET_MAX)
        return send_error(fd, 400, head_only, NULL);

    char raw_target[TARGET_MAX + 1]; /* with query, for the canonical redirect */
    snprintf(raw_target, sizeof raw_target, "%s", target);
    target[strcspn(target, "?#")] = '\0';
    char path[TARGET_MAX + 1];
    if (!url_decode(target, path, sizeof path)) return send_error(fd, 400, head_only, NULL);

    if (strcmp(path, "/healthz") == 0) {
        if (send_head(fd, 200, "text/plain; charset=utf-8", 3, "no-store", NULL) && !head_only &&
            write_all(fd, "ok\n", 3))
            *sent = 3;
        return 200;
    }

    /* Own domain: anything not addressed to CANONICAL_HOST (e.g. *.onrender.com)
     * is sent there, same path and query. /healthz above stays reachable. */
    if (canonical[0]) {
        char host[300];
        int hosts = find_host(headers, host, sizeof host);
        if (hosts > 1) return send_error(fd, 400, head_only, NULL);
        if (hosts == 0 || !host_is_canonical(host)) {
            char loc[sizeof canonical + TARGET_MAX + 32];
            snprintf(loc, sizeof loc, "Location: https://%s%s\r\n", canonical, raw_target);
            return send_error(fd, 301, head_only, loc);
        }
    }

    int seg = check_segments(path);
    if (seg) return send_error(fd, seg, head_only, NULL);

    size_t plen = strlen(path);
    bool dir_request = path[plen - 1] == '/';
    char fs[PATH_MAX];
    int n = snprintf(fs, sizeof fs, "%s%s%s", root_dir, path, dir_request ? "index.html" : "");
    if (n < 0 || (size_t)n >= sizeof fs) return send_error(fd, 404, head_only, NULL);

    char resolved[PATH_MAX];
    if (!realpath(fs, resolved)) return send_error(fd, errno == EACCES ? 403 : 404, head_only, NULL);
    if (!inside_root(resolved)) return send_error(fd, 403, head_only, NULL);

    struct stat st;
    if (stat(resolved, &st) != 0) return send_error(fd, 404, head_only, NULL);
    if (S_ISDIR(st.st_mode)) {
        if (dir_request) return send_error(fd, 404, head_only, NULL);
        /* Collapse leading slashes: "//host" would be a protocol-relative redirect. */
        const char *t = target;
        while (t[0] == '/' && t[1] == '/') t++;
        char loc[TARGET_MAX + 32];
        snprintf(loc, sizeof loc, "Location: %s/\r\n", t);
        return send_error(fd, 301, head_only, loc);
    }
    if (!S_ISREG(st.st_mode)) return send_error(fd, 404, head_only, NULL);
    return serve_file(fd, resolved, head_only, sent);
}

/* Opens the listening socket. host == NULL means "any address": a dual-stack
 * IPv6 socket (IPV6_V6ONLY off, so IPv4 clients arrive as ::ffff:a.b.c.d),
 * or plain 0.0.0.0 where the system has no IPv6. Returns fd, -1 on socket
 * errors, -2 if host is not a valid address. `shown` receives host for logs. */
static int open_listener(const char *host, long port, char *shown, size_t cap)
{
    struct sockaddr_in6 a6 = {.sin6_family = AF_INET6, .sin6_port = htons((uint16_t)port)};
    struct sockaddr_in a4 = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    int one = 1, zero = 0, fd;

    bool v6 = false;
    if (!host) {
        v6 = true;
        a6.sin6_addr = in6addr_any;
        a4.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET6, host, &a6.sin6_addr) == 1) {
        v6 = true;
    } else if (inet_pton(AF_INET, host, &a4.sin_addr) != 1) {
        return -2;
    }

    if (v6) {
        fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd >= 0) {
            if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == 0 &&
                (host || setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero) == 0) &&
                bind(fd, (struct sockaddr *)&a6, sizeof a6) == 0 && listen(fd, 128) == 0) {
                snprintf(shown, cap, "[%s]", host ? host : "::");
                return fd;
            }
            int saved = errno;
            close(fd);
            errno = saved;
        }
        if (host) return -1;
        fprintf(stderr, "server: no IPv6 (%s), falling back to 0.0.0.0\n", strerror(errno));
    }

    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0 ||
        bind(fd, (struct sockaddr *)&a4, sizeof a4) != 0 || listen(fd, 128) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    snprintf(shown, cap, "%s", host ? host : "0.0.0.0");
    return fd;
}

static void set_timeouts(int fd, int seconds)
{
    struct timeval tv = {.tv_sec = seconds, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

/* Half-close and drain, so unread request bytes do not turn into a RST
 * that could cut off the response we just wrote. */
static void finish(int fd)
{
    shutdown(fd, SHUT_WR);
    set_timeouts(fd, 1);
    char sink[4096];
    size_t total = 0;
    ssize_t n;
    while (total < 65536 && (n = read(fd, sink, sizeof sink)) > 0) total += (size_t)n;
    close(fd);
}

static void serve_client(int fd)
{
    char method[16] = "-", target[208] = "-";
    long long sent = 0;
    set_timeouts(fd, IO_TIMEOUT_S);
    int status = handle(fd, method, sizeof method, target, sizeof target, &sent);
    if (status) fprintf(stderr, "%s %s %d %lld\n", method, target, status, sent);
    finish(fd);
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : getenv("ROOT");
    if (!root || !*root) root = "public";
    const char *host = getenv("HOST");
    if (host && !*host) host = NULL;
    const char *port_s = getenv("PORT");
    if (!port_s || !*port_s) port_s = "8080";
    const char *canon = getenv("CANONICAL_HOST");
    if (canon && !set_canonical(canon)) {
        fprintf(stderr, "server: invalid CANONICAL_HOST '%s' (expected a host name like example.org)\n", canon);
        return 2;
    }

    char *end = NULL;
    errno = 0;
    long port = strtol(port_s, &end, 10);
    if (errno || *end || port < 1 || port > 65535) {
        fprintf(stderr, "server: invalid PORT '%s'\n", port_s);
        return 2;
    }
    struct stat st;
    if (!realpath(root, root_dir) || stat(root_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "server: root '%s' is not a directory\n", root);
        return 2;
    }
    root_len = strlen(root_dir);

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = {0};
    sa.sa_handler = on_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    char shown[64];
    int lfd = open_listener(host, port, shown, sizeof shown);
    if (lfd == -2) {
        fprintf(stderr, "server: invalid HOST '%s'\n", host);
        return 2;
    }
    if (lfd < 0) {
        perror("server: listen");
        return 1;
    }
    fprintf(stderr, "server: http://%s:%ld root=%s%s%s\n", shown, port, root_dir,
            canonical[0] ? " canonical=" : "", canonical);

    sigset_t chld;
    sigemptyset(&chld);
    sigaddset(&chld, SIGCHLD);

    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno != EINTR) perror("server: accept");
            continue;
        }
        if (children >= MAX_CHILDREN) {
            set_timeouts(cfd, 1);
            send_error(cfd, 503, false, "Retry-After: 1\r\n");
            close(cfd);
            continue;
        }
        sigprocmask(SIG_BLOCK, &chld, NULL);
        pid_t pid = fork();
        if (pid == 0) {
            sigprocmask(SIG_UNBLOCK, &chld, NULL);
            close(lfd);
            serve_client(cfd);
            _exit(0);
        }
        if (pid > 0) children++;
        sigprocmask(SIG_UNBLOCK, &chld, NULL);
        if (pid < 0) {
            set_timeouts(cfd, 1);
            send_error(cfd, 503, false, "Retry-After: 1\r\n");
        }
        close(cfd);
    }
}
