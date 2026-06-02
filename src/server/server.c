#include "server.h"
#include "../config/config.h"
#include "../core/context.h"
#include "../core/error.h"
#include "../core/memory.h"
#include "../filter/filter.h"
#include "../output/output.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#define HTTP_REQUEST_LIMIT 16384

typedef struct
{
    int *fds;
    int capacity;
    int head;
    int tail;
    int count;
    int closed;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
} ConnectionQueue;

typedef struct
{
    const ResolvedConfig *config;
    char **allowed_roots;
    int allowed_root_count;
    ConnectionQueue queue;
    pthread_t *threads;
    int listen_fd;
    int (*should_stop)(void *user_data);
    void *stop_user_data;
} ServerRuntime;

typedef struct
{
    OutputSink sink;
    int fd;
    int failed;
} HttpChunkedSink;

typedef struct
{
    char *root;
    char **includes;
    int include_count;
    char **excludes;
    int exclude_count;
    bool show_size;
    BinaryHandling binary_handling;
    SymlinkHandling symlink_handling;
} RequestOptions;

static int send_all(int fd, const char *data, size_t size)
{
    size_t sent = 0;
    while (sent < size)
    {
        ssize_t n = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int send_iov_all(int fd, const struct iovec *iov, int iovcnt)
{
    struct iovec local[3];
    if (!iov || iovcnt <= 0 || iovcnt > (int)(sizeof(local) / sizeof(local[0])))
        return -1;

    memcpy(local, iov, (size_t)iovcnt * sizeof(local[0]));
    struct iovec *current = local;
    int current_count = iovcnt;

    while (current_count > 0)
    {
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = current;
        msg.msg_iovlen = (size_t)current_count;

        ssize_t n = sendmsg(fd, &msg, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;

        size_t sent = (size_t)n;
        while (current_count > 0 && sent >= current[0].iov_len)
        {
            sent -= current[0].iov_len;
            current++;
            current_count--;
        }
        if (sent > 0 && current_count > 0)
        {
            current[0].iov_base = (char *)current[0].iov_base + sent;
            current[0].iov_len -= sent;
        }
    }

    return 0;
}

static int send_text_response(int fd, int status, const char *reason, const char *body)
{
    char header[512];
    size_t body_len = body ? strlen(body) : 0;
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: text/plain; charset=utf-8\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     status, reason, body_len);
    if (n < 0 || (size_t)n >= sizeof(header))
        return -1;
    if (send_all(fd, header, (size_t)n) != 0)
        return -1;
    return body_len > 0 ? send_all(fd, body, body_len) : 0;
}

static int chunked_write(OutputSink *sink, const char *data, size_t size)
{
    HttpChunkedSink *http = (HttpChunkedSink *)sink;
    if (!http || !data || http->failed)
        return -1;
    if (size == 0)
        size = strlen(data);
    if (size == 0)
        return 0;

    char prefix[32];
    int n = snprintf(prefix, sizeof(prefix), "%zx\r\n", size);
    if (n < 0 || (size_t)n >= sizeof(prefix))
    {
        http->failed = 1;
        return -1;
    }

    struct iovec iov[3] = {
        {.iov_base = prefix, .iov_len = (size_t)n},
        {.iov_base = (void *)data, .iov_len = size},
        {.iov_base = "\r\n", .iov_len = 2},
    };

    if (send_iov_all(http->fd, iov, 3) != 0)
    {
        http->failed = 1;
        return -1;
    }

    return 0;
}

static int chunked_flush(OutputSink *sink)
{
    (void)sink;
    return 0;
}

static int chunked_close(OutputSink *sink)
{
    HttpChunkedSink *http = (HttpChunkedSink *)sink;
    if (!http || http->failed)
        return -1;
    if (send_all(http->fd, "0\r\n\r\n", 5) != 0)
    {
        http->failed = 1;
        return -1;
    }
    return 0;
}

static void chunked_sink_init(HttpChunkedSink *sink, int fd)
{
    memset(sink, 0, sizeof(*sink));
    sink->sink.user_data = sink;
    sink->sink.write = chunked_write;
    sink->sink.flush = chunked_flush;
    sink->sink.close = chunked_close;
    sink->fd = fd;
}

static int queue_init(ConnectionQueue *queue, int capacity)
{
    memset(queue, 0, sizeof(*queue));
    queue->fds = calloc((size_t)capacity, sizeof(int));
    if (!queue->fds)
        return -1;
    queue->capacity = capacity;
    if (pthread_mutex_init(&queue->mutex, NULL) != 0)
    {
        free(queue->fds);
        return -1;
    }
    if (pthread_cond_init(&queue->not_empty, NULL) != 0)
    {
        pthread_mutex_destroy(&queue->mutex);
        free(queue->fds);
        return -1;
    }
    return 0;
}

static void queue_close(ConnectionQueue *queue)
{
    pthread_mutex_lock(&queue->mutex);
    queue->closed = 1;
    pthread_cond_broadcast(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
}

static void queue_destroy(ConnectionQueue *queue)
{
    if (!queue)
        return;
    for (int i = 0; i < queue->count; i++)
    {
        int idx = (queue->head + i) % queue->capacity;
        close(queue->fds[idx]);
    }
    free(queue->fds);
    pthread_cond_destroy(&queue->not_empty);
    pthread_mutex_destroy(&queue->mutex);
}

static int queue_push(ConnectionQueue *queue, int fd)
{
    int result = 0;
    pthread_mutex_lock(&queue->mutex);
    if (queue->closed || queue->count >= queue->capacity)
    {
        result = -1;
    }
    else
    {
        queue->fds[queue->tail] = fd;
        queue->tail = (queue->tail + 1) % queue->capacity;
        queue->count++;
        pthread_cond_signal(&queue->not_empty);
    }
    pthread_mutex_unlock(&queue->mutex);
    return result;
}

static int queue_pop(ConnectionQueue *queue)
{
    pthread_mutex_lock(&queue->mutex);
    while (!queue->closed && queue->count == 0)
        pthread_cond_wait(&queue->not_empty, &queue->mutex);

    if (queue->count == 0 && queue->closed)
    {
        pthread_mutex_unlock(&queue->mutex);
        return -1;
    }

    int fd = queue->fds[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    pthread_mutex_unlock(&queue->mutex);
    return fd;
}

static char from_hex(char c)
{
    if (c >= '0' && c <= '9')
        return (char)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (char)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return (char)(c - 'A' + 10);
    return -1;
}

static char *url_decode(const char *value, size_t len)
{
    char *out = malloc(len + 1);
    if (!out)
        return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++)
    {
        if (value[i] == '%' && i + 2 < len)
        {
            char hi = from_hex(value[i + 1]);
            char lo = from_hex(value[i + 2]);
            if (hi < 0 || lo < 0)
            {
                free(out);
                return NULL;
            }
            out[j++] = (char)((hi << 4) | lo);
            i += 2;
        }
        else if (value[i] == '+')
        {
            out[j++] = ' ';
        }
        else
        {
            out[j++] = value[i];
        }
    }
    out[j] = '\0';
    return out;
}

static void request_options_cleanup(RequestOptions *opts)
{
    if (!opts)
        return;
    free(opts->root);
    for (int i = 0; i < opts->include_count; i++)
        free(opts->includes[i]);
    free(opts->includes);
    for (int i = 0; i < opts->exclude_count; i++)
        free(opts->excludes[i]);
    free(opts->excludes);
    memset(opts, 0, sizeof(*opts));
}

static int append_request_string(char ***items, int *count, int max_count, char *value)
{
    if (*count >= max_count)
        return -1;
    char **new_items = realloc(*items, (size_t)(*count + 1) * sizeof(char *));
    if (!new_items)
        return -1;
    new_items[*count] = value;
    *items = new_items;
    (*count)++;
    return 0;
}

static int parse_binary_value(const char *value, BinaryHandling *out)
{
    if (strcmp(value, "skip") == 0)
        *out = BINARY_SKIP;
    else if (strcmp(value, "include") == 0)
        *out = BINARY_INCLUDE;
    else if (strcmp(value, "placeholder") == 0)
        *out = BINARY_PLACEHOLDER;
    else
        return -1;
    return 0;
}

static int parse_symlink_value(const char *value, SymlinkHandling *out)
{
    if (strcmp(value, "skip") == 0)
        *out = SYMLINK_SKIP;
    else if (strcmp(value, "follow") == 0)
        *out = SYMLINK_FOLLOW;
    else if (strcmp(value, "include") == 0)
        *out = SYMLINK_INCLUDE;
    else if (strcmp(value, "placeholder") == 0)
        *out = SYMLINK_PLACEHOLDER;
    else
        return -1;
    return 0;
}

static int parse_concat_query(const char *query, const ResolvedConfig *base, RequestOptions *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->binary_handling = base->binary_handling;
    opts->symlink_handling = base->symlink_handling;
    opts->show_size = base->show_size;

    if (!query)
        goto fail;

    const char *p = query;
    while (*p)
    {
        const char *amp = strchr(p, '&');
        size_t pair_len = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', pair_len);
        if (!eq)
            goto fail;

        char *key = url_decode(p, (size_t)(eq - p));
        char *value = url_decode(eq + 1, pair_len - (size_t)(eq - p) - 1);
        if (!key || !value)
        {
            free(key);
            free(value);
            goto fail;
        }

        int result = 0;
        if (strcmp(key, "root") == 0)
        {
            free(opts->root);
            opts->root = value;
            value = NULL;
        }
        else if (strcmp(key, "include") == 0)
        {
            result = append_request_string(&opts->includes, &opts->include_count, MAX_INCLUDES, value);
            if (result == 0)
                value = NULL;
        }
        else if (strcmp(key, "exclude") == 0)
        {
            result = append_request_string(&opts->excludes, &opts->exclude_count, MAX_EXCLUDES, value);
            if (result == 0)
                value = NULL;
        }
        else if (strcmp(key, "show_size") == 0)
        {
            opts->show_size = strcmp(value, "1") == 0 || strcmp(value, "true") == 0;
        }
        else if (strcmp(key, "binary") == 0)
        {
            result = parse_binary_value(value, &opts->binary_handling);
        }
        else if (strcmp(key, "symlinks") == 0)
        {
            result = parse_symlink_value(value, &opts->symlink_handling);
        }
        else
        {
            result = -1;
        }

        free(key);
        free(value);
        if (result != 0)
            goto fail;

        if (!amp)
            break;
        p = amp + 1;
    }

    if (!opts->root)
        goto fail;

    return 0;

fail:
    request_options_cleanup(opts);
    return -1;
}

static int path_is_under_root(const char *path, const char *root)
{
    size_t root_len = strlen(root);
    if (strcmp(path, root) == 0)
        return 1;
    return strncmp(path, root, root_len) == 0 && path[root_len] == '/';
}

static int root_allowed(ServerRuntime *runtime, const char *root)
{
    for (int i = 0; i < runtime->allowed_root_count; i++)
    {
        if (path_is_under_root(root, runtime->allowed_roots[i]))
            return 1;
    }
    return 0;
}

static int header_has_valid_auth(const char *headers, const char *token)
{
    if (!token || token[0] == '\0')
        return 1;

    const char *p = headers;
    while ((p = strstr(p, "\n")) != NULL)
    {
        p++;
        while (*p == '\r')
            p++;
        if (strncasecmp(p, "Authorization:", 14) == 0)
        {
            p += 14;
            while (*p == ' ' || *p == '\t')
                p++;
            const char *prefix = "Bearer ";
            size_t prefix_len = strlen(prefix);
            if (strncmp(p, prefix, prefix_len) != 0)
                return 0;
            p += prefix_len;
            size_t token_len = strlen(token);
            return strncmp(p, token, token_len) == 0 &&
                   (p[token_len] == '\r' || p[token_len] == '\n');
        }
    }

    return 0;
}

static int read_http_request(int fd, char *buffer, size_t size)
{
    size_t total = 0;
    while (total + 1 < size)
    {
        ssize_t n = recv(fd, buffer + total, size - total - 1, 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            break;
        total += (size_t)n;
        buffer[total] = '\0';
        if (strstr(buffer, "\r\n\r\n") || strstr(buffer, "\n\n"))
            return 0;
    }
    return -1;
}

static void handle_concat(ServerRuntime *runtime, int fd, const char *query)
{
    RequestOptions opts;
    if (parse_concat_query(query, runtime->config, &opts) != 0)
    {
        send_text_response(fd, 400, "Bad Request", "bad concat query\n");
        return;
    }

    char *root_real = realpath(opts.root, NULL);
    if (!root_real)
    {
        request_options_cleanup(&opts);
        send_text_response(fd, 404, "Not Found", "root not found\n");
        return;
    }

    struct stat st;
    if (stat(root_real, &st) != 0 || !S_ISDIR(st.st_mode) || !root_allowed(runtime, root_real))
    {
        free(root_real);
        request_options_cleanup(&opts);
        send_text_response(fd, 403, "Forbidden", "root not allowed\n");
        return;
    }

    ResolvedConfig req = {0};
    req.mode = FCONCAT_MODE_BATCH;
    req.input_directory = root_real;
    req.binary_handling = opts.binary_handling;
    req.symlink_handling = opts.symlink_handling;
    req.show_size = opts.show_size;
    req.log_level = runtime->config->log_level;
    req.verbose = runtime->config->verbose;
    req.include_patterns = opts.includes;
    req.include_count = opts.include_count;
    req.exclude_patterns = opts.excludes;
    req.exclude_count = opts.exclude_count;
    opts.includes = NULL;
    opts.excludes = NULL;
    opts.include_count = 0;
    opts.exclude_count = 0;

    if (send_all(fd,
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/plain; charset=utf-8\r\n"
                 "Transfer-Encoding: chunked\r\n"
                 "Connection: close\r\n\r\n",
                 strlen("HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/plain; charset=utf-8\r\n"
                        "Transfer-Encoding: chunked\r\n"
                        "Connection: close\r\n\r\n")) != 0)
    {
        resolved_config_cleanup(&req);
        request_options_cleanup(&opts);
        return;
    }

    ErrorManager *errors = error_manager_create();
    MemoryManager *memory = memory_manager_create();
    FilterEngine *filters = filter_engine_create();
    HttpChunkedSink sink;
    chunked_sink_init(&sink, fd);
    BufferedOutputSink buffered_sink;
    OutputSink *response_sink = &sink.sink;
    int using_buffered_sink = buffered_output_sink_init(&buffered_sink, &sink.sink, 0, 1) == 0;
    if (using_buffered_sink)
        response_sink = &buffered_sink.sink;

    if (errors && memory && filters && filter_engine_configure(filters, &req) == 0)
    {
        ProcessingStats stats = {0};
        FconcatContext *ctx = create_fconcat_context(&req, response_sink, &stats, errors, memory, filters);
        if (ctx)
        {
            (void)process_fconcat_document(ctx, &req, runtime->should_stop, runtime->stop_user_data);
            destroy_fconcat_context(ctx);
        }
    }

    if (!sink.failed)
        (void)output_sink_close(response_sink);
    if (using_buffered_sink)
        buffered_output_sink_destroy(&buffered_sink);

    if (filters)
        filter_engine_destroy(filters);
    if (memory)
        memory_manager_destroy(memory);
    if (errors)
        error_manager_destroy(errors);
    resolved_config_cleanup(&req);
    request_options_cleanup(&opts);
}

static void handle_client(ServerRuntime *runtime, int fd)
{
    char request[HTTP_REQUEST_LIMIT];
    if (read_http_request(fd, request, sizeof(request)) != 0)
    {
        send_text_response(fd, 400, "Bad Request", "bad request\n");
        return;
    }

    char *line_end = strstr(request, "\r\n");
    if (!line_end)
        line_end = strchr(request, '\n');
    if (!line_end)
    {
        send_text_response(fd, 400, "Bad Request", "missing request line\n");
        return;
    }
    *line_end = '\0';

    char *method = request;
    char *target = strchr(method, ' ');
    if (!target)
    {
        send_text_response(fd, 400, "Bad Request", "bad request line\n");
        return;
    }
    *target++ = '\0';
    char *version = strchr(target, ' ');
    if (!version)
    {
        send_text_response(fd, 400, "Bad Request", "bad request line\n");
        return;
    }
    *version++ = '\0';

    if (strcmp(method, "GET") != 0)
    {
        send_text_response(fd, 405, "Method Not Allowed", "method not allowed\n");
        return;
    }

    if (!header_has_valid_auth(line_end + 1, runtime->config->auth_token))
    {
        send_text_response(fd, 401, "Unauthorized", "unauthorized\n");
        return;
    }

    char *query = strchr(target, '?');
    if (query)
        *query++ = '\0';

    if (strcmp(target, "/healthz") == 0)
    {
        send_text_response(fd, 200, "OK", "ok\n");
    }
    else if (strcmp(target, "/concat") == 0)
    {
        handle_concat(runtime, fd, query);
    }
    else
    {
        send_text_response(fd, 404, "Not Found", "not found\n");
    }
}

static void *worker_main(void *arg)
{
    ServerRuntime *runtime = (ServerRuntime *)arg;
    for (;;)
    {
        int fd = queue_pop(&runtime->queue);
        if (fd < 0)
            break;
        handle_client(runtime, fd);
        close(fd);
    }
    return NULL;
}

static int create_listener(const char *host, int port)
{
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_buf, &hints, &res) != 0)
        return -1;

    int fd = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next)
    {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;

        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(fd, p->ai_addr, p->ai_addrlen) == 0 && listen(fd, SOMAXCONN) == 0)
            break;

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

static void free_allowed_roots(ServerRuntime *runtime)
{
    for (int i = 0; i < runtime->allowed_root_count; i++)
        free(runtime->allowed_roots[i]);
    free(runtime->allowed_roots);
    runtime->allowed_roots = NULL;
    runtime->allowed_root_count = 0;
}

static int prepare_allowed_roots(ServerRuntime *runtime)
{
    runtime->allowed_roots = calloc((size_t)runtime->config->allow_root_count, sizeof(char *));
    if (!runtime->allowed_roots)
        return -1;

    for (int i = 0; i < runtime->config->allow_root_count; i++)
    {
        char *real = realpath(runtime->config->allow_roots[i], NULL);
        if (!real)
            return -1;
        runtime->allowed_roots[runtime->allowed_root_count++] = real;
    }
    return 0;
}

int server_run(const ResolvedConfig *config, int (*should_stop)(void *user_data), void *user_data)
{
    if (!config || !config->listen_host || config->listen_port <= 0 || config->allow_root_count <= 0)
        return -1;

    ServerRuntime runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.config = config;
    runtime.listen_fd = -1;
    runtime.should_stop = should_stop;
    runtime.stop_user_data = user_data;

    if (prepare_allowed_roots(&runtime) != 0)
    {
        free_allowed_roots(&runtime);
        return -1;
    }

    if (queue_init(&runtime.queue, config->server_queue_size) != 0)
    {
        free_allowed_roots(&runtime);
        return -1;
    }

    runtime.threads = calloc((size_t)config->server_workers, sizeof(pthread_t));
    if (!runtime.threads)
    {
        queue_destroy(&runtime.queue);
        free_allowed_roots(&runtime);
        return -1;
    }

    runtime.listen_fd = create_listener(config->listen_host, config->listen_port);
    if (runtime.listen_fd < 0)
    {
        free(runtime.threads);
        queue_destroy(&runtime.queue);
        free_allowed_roots(&runtime);
        return -1;
    }

    for (int i = 0; i < config->server_workers; i++)
    {
        if (pthread_create(&runtime.threads[i], NULL, worker_main, &runtime) != 0)
        {
            queue_close(&runtime.queue);
            for (int j = 0; j < i; j++)
                pthread_join(runtime.threads[j], NULL);
            close(runtime.listen_fd);
            free(runtime.threads);
            queue_destroy(&runtime.queue);
            free_allowed_roots(&runtime);
            return -1;
        }
    }

    printf("fconcat server listening on %s:%d\n", config->listen_host, config->listen_port);
    fflush(stdout);

    while (!should_stop || !should_stop(user_data))
    {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(runtime.listen_fd, &readfds);
        struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
        int ready = select(runtime.listen_fd + 1, &readfds, NULL, NULL, &timeout);
        if (ready < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready == 0)
            continue;

        int client = accept(runtime.listen_fd, NULL, NULL);
        if (client < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }

        if (queue_push(&runtime.queue, client) != 0)
        {
            send_text_response(client, 503, "Service Unavailable", "server queue full\n");
            close(client);
        }
    }

    close(runtime.listen_fd);
    queue_close(&runtime.queue);
    for (int i = 0; i < config->server_workers; i++)
        pthread_join(runtime.threads[i], NULL);

    free(runtime.threads);
    queue_destroy(&runtime.queue);
    free_allowed_roots(&runtime);
    return 0;
}
