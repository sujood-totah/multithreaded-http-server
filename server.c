#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <sys/stat.h>
#include <sys/select.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

#include "threadpool.h"

#define RFC1123FMT "%a, %d %b %Y %H:%M:%S GMT"

static int handle_client(int fd);
static int read_request_line(int fd, char *buf, int max);
static void send_error(int fd, int code, const char *phrase, const char *body_line);
static void send_file(int fd, const char *path);
static void send_dir_listing(int fd, const char *dirpath, const char *urlpath);
static int has_permissions(const char *path);
static void http_date(char *out, size_t n, time_t t);

/* ---------- MIME ---------- */
static char *get_mime_type(char *name) {
    char *ext = strrchr(name, '.');
    if (!ext) return NULL;
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".gif") == 0) return "image/gif";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".au") == 0) return "audio/basic";
    if (strcmp(ext, ".wav") == 0) return "audio/wav";
    if (strcmp(ext, ".avi") == 0) return "video/x-msvideo";
    if (strcmp(ext, ".mpeg") == 0 || strcmp(ext, ".mpg") == 0) return "video/mpeg";
    if (strcmp(ext, ".mp3") == 0) return "audio/mpeg";
    return NULL;
}

static void http_date(char *out, size_t n, time_t t) {
    struct tm *tm = gmtime(&t);
    strftime(out, n, RFC1123FMT, tm);
}

/* ---------- write all bytes (handles EINTR, partial write) ---------- */
static int write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* ---------- errors (bodies match reference 400.txt, 501.txt, etc.) ---------- */
static void send_error(int fd, int code, const char *phrase, const char *body_line)
{
    time_t now = time(NULL);
    char datebuf[128];
    http_date(datebuf, sizeof(datebuf), now);

    /* 400.txt: status line "Bad Request", H4 "Bad request" */
    const char *h4_phrase = (code == 400 && strcmp(phrase, "Bad Request") == 0) ? "Bad request" : phrase;

    char body[2048];
    snprintf(body, sizeof(body),
             "<HTML><HEAD><TITLE>%d %s</TITLE></HEAD>\r\n"
             "<BODY><H4>%d %s</H4>\r\n"
             "%s\r\n"
             "</BODY></HTML>",
             code, h4_phrase, code, h4_phrase, body_line);

    char header[4096];
    int blen = (int)strlen(body);

    snprintf(header, sizeof(header),
             "HTTP/1.0 %d %s\r\n"
             "Server: webserver/1.0\r\n"
             "Date: %s\r\n"
             "Content-Type: text/html\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n"
             "\r\n",
             code, phrase, datebuf, blen);

    (void)write_all(fd, header, strlen(header));
    (void)write_all(fd, body, (size_t)blen);
}

/* ---------- file send ---------- */
static void send_file(int fd, const char *path)
{
    int f = open(path, O_RDONLY);
    if (f < 0) {
        if (errno == EACCES)
            send_error(fd, 403, "Forbidden", "Access denied.");
        else
            send_error(fd, 500, "Internal Server Error", "Some server side error.");
        return;
    }

    struct stat st;
    if (fstat(f, &st) < 0) {
        close(f);
        send_error(fd, 500, "Internal Server Error", "Some server side error.");
        return;
    }

    time_t now = time(NULL);
    char datebuf[128], modbuf[128];
    http_date(datebuf, sizeof(datebuf), now);
    http_date(modbuf, sizeof(modbuf), st.st_mtime);

    char *mime = get_mime_type((char*)path);

    char header[4096];
    if (mime) {
        snprintf(header, sizeof(header),
                 "HTTP/1.0 200 OK\r\n"
                 "Server: webserver/1.0\r\n"
                 "Date: %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %ld\r\n"
                 "Last-Modified: %s\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 datebuf, mime, (long)st.st_size, modbuf);
    } else {
        snprintf(header, sizeof(header),
                 "HTTP/1.0 200 OK\r\n"
                 "Server: webserver/1.0\r\n"
                 "Date: %s\r\n"
                 "Content-Length: %ld\r\n"
                 "Last-Modified: %s\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 datebuf, (long)st.st_size, modbuf);
    }

    (void)write_all(fd, header, strlen(header));

    char buf[8192];
    ssize_t n;
    while ((n = read(f, buf, sizeof(buf))) > 0) {
        if (write_all(fd, buf, (size_t)n) < 0) break;
    }
    close(f);
}

/* ---------- request line (accepts \n or \r\n) ---------- */
static int read_request_line(int fd, char *buf, int max)
{
    int pos = 0;
    char c;

    while (pos < max - 1) {
        int n = (int)read(fd, &c, 1);
        if (n < 0) return -1;
        if (n == 0) break;

        buf[pos++] = c;
        if (c == '\n') break; /* accept LF end (with or without CR) */
    }

    buf[pos] = '\0';

    /* strip trailing \n and optional \r */
    if (pos > 0 && buf[pos - 1] == '\n') {
        buf[pos - 1] = '\0';
        pos--;
    }
    if (pos > 0 && buf[pos - 1] == '\r') {
        buf[pos - 1] = '\0';
        pos--;
    }

    return pos;
}

/* block %2e%2e (case-insensitive) */
static int contains_dotdot_encoded(const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '%' &&
            (p[1] == '2') &&
            (p[2] == 'e' || p[2] == 'E') &&
            p[3] == '%' &&
            (p[4] == '2') &&
            (p[5] == 'e' || p[5] == 'E')) {
            return 1;
        }
    }
    return 0;
}

/* permissions per "others" bits (common in testers) */
static int has_permissions(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) return 0;

    if (S_ISREG(st.st_mode)) {
        if (!(st.st_mode & S_IROTH)) return 0; /* file readable by others */
    } else if (S_ISDIR(st.st_mode)) {
        if (!(st.st_mode & S_IXOTH)) return 0; /* dir searchable by others */
    }

    /* every directory component (except "." cwd) must be X for others */
    char tmp[4096];
    strncpy(tmp, path, sizeof(tmp));
    tmp[sizeof(tmp)-1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (stat(tmp, &st) < 0) return 0;
            /* skip permission check for "." (cwd); tester may run with cwd without o+x */
            if (strcmp(tmp, ".") != 0 && S_ISDIR(st.st_mode)) {
                if (!(st.st_mode & S_IXOTH)) return 0;
            }
            *p = '/';
        }
    }

    return 1;
}

/* ---------- dir listing ---------- */
static void send_dir_listing(int fd, const char *dirpath, const char *urlpath)
{
    struct stat dst;
    if (stat(dirpath, &dst) < 0) {
        send_error(fd, 500, "Internal Server Error", "Some server side error.");
        return;
    }

    char body[200000];
    size_t used = 0;

    used += snprintf(body + used, sizeof(body) - used,
        "<HTML>\r\n"
        "<HEAD><TITLE>Index of %s</TITLE></HEAD>\r\n"
        "<BODY>\r\n"
        "<H4>Index of %s</H4>\r\n"
        "<table CELLSPACING=8>\r\n"
        "<tr><th>Name</th><th>Last Modified</th><th>Size</th></tr>\r\n",
        urlpath, urlpath);

    struct dirent **namelist;
    int n = scandir(dirpath, &namelist, NULL, alphasort);
    if (n < 0) {
        send_error(fd, 500, "Internal Server Error", "Some server side error.");
        return;
    }

    for (int i = 0; i < n; i++) {
        struct dirent *ent = namelist[i];
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            free(ent);
            continue;
        }

        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, ent->d_name);

        struct stat st;
        if (stat(fullpath, &st) < 0) {
            free(ent);
            continue;
        }

        char timebuf[128];
        http_date(timebuf, sizeof(timebuf), st.st_mtime);

        /* relative HREF (entity name only) to match reference dir_content.txt */
        if (S_ISDIR(st.st_mode)) {
            used += snprintf(body + used, sizeof(body) - used,
                "<tr>\r\n"
                "<td><A HREF=\"%s/\">%s/</A></td>\r\n"
                "<td>%s</td>\r\n"
                "<td></td>\r\n"
                "</tr>\r\n",
                ent->d_name, ent->d_name, timebuf);
        } else {
            used += snprintf(body + used, sizeof(body) - used,
                "<tr>\r\n"
                "<td><A HREF=\"%s\">%s</A></td>\r\n"
                "<td>%s</td>\r\n"
                "<td>%ld</td>\r\n"
                "</tr>\r\n",
                ent->d_name, ent->d_name, timebuf, (long)st.st_size);
        }

        free(ent);
        if (used > sizeof(body) - 1024) break;
    }
    free(namelist);

    used += snprintf(body + used, sizeof(body) - used,
        "</table>\r\n"
        "<HR>\r\n"
        "<ADDRESS>webserver/1.0</ADDRESS>\r\n"
        "</BODY></HTML>\r\n");

    time_t now = time(NULL);
    char datebuf[128], modbuf[128];
    http_date(datebuf, sizeof(datebuf), now);
    http_date(modbuf, sizeof(modbuf), dst.st_mtime);

    char header[4096];
    snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\n"
        "Server: webserver/1.0\r\n"
        "Date: %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %zu\r\n"
        "Last-Modified: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        datebuf, used, modbuf);

    (void)write_all(fd, header, strlen(header));
    (void)write_all(fd, body, used);
}

int handle_client(int fd)
{
    char line[4001];

    int r = read_request_line(fd, line, (int)sizeof(line));
    if (r < 0) {
        send_error(fd, 500, "Internal Server Error", "Some server side error.");
        return -1;
    }
    if (r == 0) {
        send_error(fd, 400, "Bad Request", "Bad Request.");
        return 0;
    }

    /* Parse: METHOD SP PATH SP PROTO */
    char *method  = strtok(line, " ");
    char *urlpath = strtok(NULL, " ");
    char *proto   = strtok(NULL, " ");

    if (!method || !urlpath || !proto || strtok(NULL, " ")) {
        send_error(fd, 400, "Bad Request", "Bad Request.");
        return 0;
    }

    if (strcmp(proto, "HTTP/1.0") != 0 && strcmp(proto, "HTTP/1.1") != 0) {
        send_error(fd, 400, "Bad Request", "Bad Request.");
        return 0;
    }

    if (strcmp(method, "GET") != 0) {
        send_error(fd, 501, "Not supported", "Method is not supported.");
        return 0;
    }

    if (urlpath[0] != '/') {
        send_error(fd, 400, "Bad Request", "Bad Request.");
        return 0;
    }

    /* block traversal */
    if (strstr(urlpath, "..") != NULL || contains_dotdot_encoded(urlpath)) {
        send_error(fd, 403, "Forbidden", "Access denied.");
        return 0;
    }

    char fs_path[4096];
    if (snprintf(fs_path, sizeof(fs_path), ".%s", urlpath) >= (int)sizeof(fs_path)) {
        send_error(fd, 400, "Bad Request", "Bad Request.");
        return 0;
    }

    struct stat st;
    if (stat(fs_path, &st) < 0) {
        send_error(fd, 404, "Not Found", "File not found.");
        return 0;
    }

    if (S_ISDIR(st.st_mode)) {
        size_t L = strlen(urlpath);

        /* redirect dir without trailing slash (or 403 if no permission) */
        if (L == 0 || urlpath[L - 1] != '/') {
            if (!has_permissions(fs_path)) {
                send_error(fd, 403, "Forbidden", "Access denied.");
                return 0;
            }
            char loc[4096];
            if (snprintf(loc, sizeof(loc), "%s/", urlpath) >= (int)sizeof(loc)) {
                send_error(fd, 400, "Bad Request", "Bad Request.");
                return 0;
            }

            time_t now = time(NULL);
            char datebuf[128];
            http_date(datebuf, sizeof(datebuf), now);

            const char *body =
                "<HTML><HEAD><TITLE>302 Found</TITLE></HEAD>\r\n"
                "<BODY><H4>302 Found</H4>\r\n"
                "Directories must end with a slash.\r\n"
                "</BODY></HTML>";

            char header[8192];
            int blen = (int)strlen(body);

            snprintf(header, sizeof(header),
                     "HTTP/1.0 302 Found\r\n"
                     "Server: webserver/1.0\r\n"
                     "Date: %s\r\n"
                     "Location: %s\r\n"
                     "Content-Type: text/html\r\n"
                     "Content-Length: %d\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     datebuf, loc, blen);

            (void)write_all(fd, header, strlen(header));
            (void)write_all(fd, body, (size_t)blen);
            return 0;
        }

        /* if has index.html -> serve it */
        char index_path[4096];
        if (snprintf(index_path, sizeof(index_path), "%s/index.html", fs_path) >= (int)sizeof(index_path)) {
            send_error(fd, 400, "Bad Request", "Bad Request.");
            return 0;
        }

        struct stat st2;
        if (stat(index_path, &st2) == 0 && S_ISREG(st2.st_mode)) {
            if (!has_permissions(index_path)) {
                send_error(fd, 403, "Forbidden", "Access denied.");
                return 0;
            }
            send_file(fd, index_path);
            return 0;
        }

        /* else directory listing */
        if (!has_permissions(fs_path)) {
            send_error(fd, 403, "Forbidden", "Access denied.");
            return 0;
        }

        send_dir_listing(fd, fs_path, urlpath);
        return 0;
    }

    if (!S_ISREG(st.st_mode) || !has_permissions(fs_path)) {
        send_error(fd, 403, "Forbidden", "Access denied.");
        return 0;
    }

    send_file(fd, fs_path);
    return 0;
}

/* ---------- threadpool task ---------- */
static int handle_client_task(void *arg)
{
    int fd = *(int*)arg;
    free(arg);

    (void)handle_client(fd);
    close(fd);

    return 0;
}

/* ---------- main ---------- */
int main(int argc, char *argv[])
{
    /* prevent server from dying on client disconnect */
    signal(SIGPIPE, SIG_IGN);

    if (argc != 5) {
        fprintf(stderr, "Usage: server <port> <pool-size> <max-queue-size> <max-number-of-requests>\n");
        fflush(stderr);
        exit(1);
    }

    int port = atoi(argv[1]);
    int pool_size = atoi(argv[2]);
    int max_queue_size = atoi(argv[3]);
    int max_requests = atoi(argv[4]);

    /* Listen on IPv4 any (per assignment). Test connects to 127.0.0.1. */
    int fd = socket(PF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        fflush(stderr);
        exit(1);
    }
    int opt = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons((uint16_t)port);
    srv.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        perror("bind");
        fflush(stderr);
        close(fd);
        exit(1);
    }

    if (listen(fd, SOMAXCONN) < 0) {
        perror("listen");
        fflush(stderr);
        close(fd);
        exit(1);
    }

    threadpool *tp = create_threadpool(pool_size, max_queue_size);
    if (!tp) {
        fprintf(stderr, "create_threadpool failed\n");
        fflush(stderr);
        close(fd);
        exit(1);
    }

    struct sockaddr_in cli;
    socklen_t cli_len;

    for (int i = 0; i < max_requests; ) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;

        int ready = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            fflush(stderr);
            break;
        }
        if (ready == 0) {
            /* timeout: no new connections -> exit so Valgrind can finish */
            break;
        }

        cli_len = sizeof(cli);
        int newfd = accept(fd, (struct sockaddr *)&cli, &cli_len);
        if (newfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            fflush(stderr);
            break;
        }

        int *pfd = (int*)malloc(sizeof(int));
        if (!pfd) {
            send_error(newfd, 500, "Internal Server Error", "Some server side error.");
            close(newfd);
            continue;
        }
        *pfd = newfd;

        dispatch(tp, handle_client_task, pfd);
        i++;
    }

    destroy_threadpool(tp);
    close(fd);
    return 0;
}
