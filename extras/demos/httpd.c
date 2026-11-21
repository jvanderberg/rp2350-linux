/*
 * httpd.c - tiny static file server.
 *
 *   cc -Wall -o httpd httpd.c
 *   ./httpd [-p port] [dir]        (default: port 8080, current directory)
 *
 * One client at a time, no fork (fine without an MMU), GET and HEAD only.
 * Directories serve index.html if present, otherwise a listing.
 * Any path component starting with '.' is refused, which blocks both
 * ".." escapes and dotfiles such as .ssh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define REQ_MAX  2048
#define IO_BUF   4096
#define TIMEOUT  10             /* seconds a client may stall */

static const struct { const char *ext, *type; } mime[] = {
    { "html", "text/html; charset=utf-8" },
    { "htm",  "text/html; charset=utf-8" },
    { "css",  "text/css" },
    { "js",   "text/javascript" },
    { "json", "application/json" },
    { "txt",  "text/plain; charset=utf-8" },
    { "c",    "text/plain; charset=utf-8" },
    { "h",    "text/plain; charset=utf-8" },
    { "sh",   "text/plain; charset=utf-8" },
    { "py",   "text/plain; charset=utf-8" },
    { "md",   "text/plain; charset=utf-8" },
    { "svg",  "image/svg+xml" },
    { "png",  "image/png" },
    { "jpg",  "image/jpeg" },
    { "jpeg", "image/jpeg" },
    { "gif",  "image/gif" },
    { "ico",  "image/x-icon" },
    { "pdf",  "application/pdf" },
    { "wasm", "application/wasm" },
};

static const char *mime_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    size_t i;

    if (dot && (!slash || dot > slash))
        for (i = 0; i < sizeof mime / sizeof mime[0]; i++)
            if (!strcasecmp(dot + 1, mime[i].ext))
                return mime[i].type;
    return "application/octet-stream";
}

static int send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Short text response; also used for redirects via the extra header. */
static int reply(int fd, int code, const char *reason, const char *extra,
                 int head_only)
{
    char buf[512];
    int n = snprintf(buf, sizeof buf,
                     "HTTP/1.0 %d %s\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: %d\r\n"
                     "%s"
                     "Connection: close\r\n\r\n"
                     "%s\n",
                     code, reason, (int)strlen(reason) + 1,
                     extra ? extra : "", reason);
    if (n >= (int)sizeof buf)
        n = sizeof buf - 1;
    if (head_only)
        n -= (int)strlen(reason) + 1;
    send_all(fd, buf, (size_t)n);
    return code;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Decode %XX in place, dropping any query string. Returns -1 if malformed. */
static int url_decode(char *s)
{
    char *o = s;

    for (; *s && *s != '?' && *s != '#'; s++) {
        if (*s == '%') {
            int hi = hexval((unsigned char)s[1]);
            int lo = hi < 0 ? -1 : hexval((unsigned char)s[2]);
            if (lo < 0 || (hi == 0 && lo == 0))
                return -1;
            *o++ = (char)(hi << 4 | lo);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
    return 0;
}

static void put_html(FILE *f, const char *s)
{
    for (; *s; s++)
        switch (*s) {
        case '<': fputs("&lt;", f); break;
        case '>': fputs("&gt;", f); break;
        case '&': fputs("&amp;", f); break;
        case '"': fputs("&quot;", f); break;
        default:  fputc(*s, f);
        }
}

static void put_url(FILE *f, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')
            || c == '-' || c == '_' || c == '.' || c == '~')
            fputc(c, f);
        else
            fprintf(f, "%%%02X", c);
    }
}

static int no_dotfiles(const struct dirent *d)
{
    return d->d_name[0] != '.';
}

static int send_listing(int fd, const char *fspath, const char *urlpath,
                        int head_only)
{
    static const char hdr[] =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n";
    struct dirent **list;
    int i, n, dfd;
    FILE *f;

    n = scandir(fspath, &list, no_dotfiles, alphasort);
    if (n < 0)
        return reply(fd, 404, "Not Found", NULL, head_only);

    send_all(fd, hdr, sizeof hdr - 1);
    dfd = head_only ? -1 : dup(fd);
    f = dfd < 0 ? NULL : fdopen(dfd, "w");
    if (!f && dfd >= 0)
        close(dfd);

    if (f) {
        fputs("<!doctype html><meta name=viewport content=\"width=device-width\">"
              "<title>", f);
        put_html(f, urlpath);
        fputs("</title><h1>", f);
        put_html(f, urlpath);
        fputs("</h1><pre>", f);
        if (strcmp(urlpath, "/"))
            fputs("<a href=\"../\">../</a>\n", f);
    }
    for (i = 0; i < n; i++) {
        if (f) {
            char full[1024];
            struct stat st;
            int isdir;

            snprintf(full, sizeof full, "%s/%s", fspath, list[i]->d_name);
            if (stat(full, &st) == 0) {
                isdir = S_ISDIR(st.st_mode);
                fputs("<a href=\"", f);
                put_url(f, list[i]->d_name);
                fputs(isdir ? "/\">" : "\">", f);
                put_html(f, list[i]->d_name);
                if (isdir)
                    fputs("/</a>\n", f);
                else
                    fprintf(f, "</a>  %ld\n", (long)st.st_size);
            }
        }
        free(list[i]);
    }
    free(list);
    if (f) {
        fputs("</pre>\n", f);
        fclose(f);
    }
    return 200;
}

static int send_file(int fd, const char *fspath, const struct stat *st,
                     int head_only)
{
    char buf[IO_BUF];
    ssize_t r;
    int n, in = open(fspath, O_RDONLY);

    if (in < 0)
        return reply(fd, errno == EACCES ? 403 : 404,
                     errno == EACCES ? "Forbidden" : "Not Found",
                     NULL, head_only);

    n = snprintf(buf, sizeof buf,
                 "HTTP/1.0 200 OK\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %ld\r\n"
                 "Connection: close\r\n\r\n",
                 mime_type(fspath), (long)st->st_size);
    if (send_all(fd, buf, (size_t)n) == 0 && !head_only)
        while ((r = read(in, buf, sizeof buf)) > 0)
            if (send_all(fd, buf, (size_t)r) < 0)
                break;
    close(in);
    return 200;
}

/* Handle one request; returns the status code for the log (0 = no request). */
static int serve(int fd, char *req, char **path_out)
{
    char fspath[REQ_MAX + 16];
    struct stat st;
    size_t len = 0, plen;
    char *method, *path, *end;
    int head_only;

    *path_out = "-";
    for (;;) {
        ssize_t r = read(fd, req + len, REQ_MAX - 1 - len);
        if (r <= 0)
            return 0;
        len += (size_t)r;
        req[len] = 0;
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n"))
            break;
        if (len >= REQ_MAX - 1)
            return reply(fd, 431, "Request Too Large", NULL, 0);
    }

    method = req;
    path = strchr(req, ' ');
    if (!path)
        return reply(fd, 400, "Bad Request", NULL, 0);
    *path++ = 0;
    end = path + strcspn(path, " \r\n");
    *end = 0;

    head_only = !strcmp(method, "HEAD");
    if (!head_only && strcmp(method, "GET"))
        return reply(fd, 405, "Method Not Allowed", "Allow: GET, HEAD\r\n", 0);

    if (path[0] != '/' || url_decode(path) < 0)
        return reply(fd, 400, "Bad Request", NULL, head_only);
    *path_out = path;
    if (strstr(path, "/."))
        return reply(fd, 404, "Not Found", NULL, head_only);

    snprintf(fspath, sizeof fspath, ".%s", path);
    if (stat(fspath, &st) < 0)
        return reply(fd, 404, "Not Found", NULL, head_only);

    if (S_ISDIR(st.st_mode)) {
        plen = strlen(path);
        if (path[plen - 1] != '/') {
            /* Redirect so relative links in the directory resolve. */
            FILE *f;
            int dfd = dup(fd);
            if (dfd >= 0 && (f = fdopen(dfd, "w"))) {
                fputs("HTTP/1.0 301 Moved Permanently\r\nLocation: ", f);
                put_url(f, "");
                for (end = path; *end; end++)
                    if (*end == '/')
                        fputc('/', f);
                    else {
                        char one[2] = { *end, 0 };
                        put_url(f, one);
                    }
                fputs("/\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", f);
                fclose(f);
            } else if (dfd >= 0) {
                close(dfd);
            }
            return 301;
        }
        strcat(fspath, "index.html");
        if (stat(fspath, &st) == 0 && S_ISREG(st.st_mode))
            return send_file(fd, fspath, &st, head_only);
        fspath[strlen(fspath) - 10] = 0;
        return send_listing(fd, fspath, path, head_only);
    }
    if (!S_ISREG(st.st_mode))
        return reply(fd, 403, "Forbidden", NULL, head_only);
    return send_file(fd, fspath, &st, head_only);
}

int main(int argc, char **argv)
{
    static char req[REQ_MAX];
    struct sockaddr_in addr;
    struct timeval tv = { TIMEOUT, 0 };
    const char *dir = ".";
    int port = 8080, quiet = 0, one = 1, opt, srv;

    while ((opt = getopt(argc, argv, "p:q")) != -1)
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'q': quiet = 1; break;
        default:
            fprintf(stderr, "usage: %s [-p port] [-q] [dir]\n", argv[0]);
            return 2;
        }
    if (optind < argc)
        dir = argv[optind];
    if (chdir(dir) < 0) {
        perror(dir);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0
        || listen(srv, 8) < 0) {
        perror("bind/listen");
        return 1;
    }
    if (!quiet) {
        printf("serving %s on port %d\n", dir, port);
        fflush(stdout);
    }

    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        char *path;
        int code, fd = accept(srv, (struct sockaddr *)&peer, &plen);

        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            perror("accept");
            sleep(1);
            continue;
        }
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        code = serve(fd, req, &path);
        close(fd);
        if (!quiet && code) {
            printf("%s %d %s\n", inet_ntoa(peer.sin_addr), code, path);
            fflush(stdout);
        }
    }
}
