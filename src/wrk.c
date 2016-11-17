// Copyright (C) 2012 - Will Glozer.  All rights reserved.

#include "wrk.h"
#include "script.h"
#include "main.h"

/* 配置信息，一部分来自命令行参数 */
static struct config {
    uint64_t connections;          /* 对应参数-c，默认值10 */
    uint64_t duration;             /* 对应参数-d，默认值10s */
    uint64_t threads;              /* 参数-t，默认值2 */
    uint64_t timeout;              /* 参数--timeout，默认2000ms */
    uint64_t pipeline;             /* 请求报文个数 */
    bool     delay;                /* 是否设置了全局的delay()函数 */
    bool     dynamic;              /* 报文是否为动态生成，通过判断是否有全局request()函数；注意此处的全局不是wrk.request() */
    bool     latency;              /* 参数--latency */
    char    *host;                 /* 目标地址的域名或IP地址 */
    char    *script;               /* Lua脚本路径，通过命令行参数-s传入 */
    SSL_CTX *ctx;                  /* SSL环境 */
} cfg;

/* 统计结果 */
static struct {
    stats *latency;                /* 响应延迟(从发送请求到响应结束), 每报文统计，索引为延迟的us数 */
    stats *requests;               /* 请求速率，每100ms统计一次 */
} statistics;

static struct sock sock = {
    .connect  = sock_connect,
    .close    = sock_close,
    .read     = sock_read,
    .write    = sock_write,
    .readable = sock_readable
};

/* 报文处理句柄组 */
static struct http_parser_settings parser_settings = {
    .on_message_complete = response_complete
};

/* 全局标识，运行周期结束，进程停止运行退出 */
static volatile sig_atomic_t stop = 0;

/* SIGINT处理句柄 */
static void handler(int sig) {
    stop = 1;
}

static void usage() {
    printf("Usage: wrk <options> <url>                            \n"
           "  Options:                                            \n"
           "    -c, --connections <N>  Connections to keep open   \n"
           "    -d, --duration    <T>  Duration of test           \n"
           "    -t, --threads     <N>  Number of threads to use   \n"
           "                                                      \n"
           "    -s, --script      <S>  Load Lua script file       \n"
           "    -H, --header      <H>  Add header to request      \n"
           "        --latency          Print latency statistics   \n"
           "        --timeout     <T>  Socket/request timeout     \n"
           "    -v, --version          Print version details      \n"
           "                                                      \n"
           "  Numeric arguments may include a SI unit (1k, 1M, 1G)\n"
           "  Time arguments may include a time unit (2s, 2m, 2h)\n");
}

/* 主main入口函数 */
int main(int argc, char **argv) {
    char *url, **headers = zmalloc(argc * sizeof(char *));
    struct http_parser_url parts = {};     /* 盛放URL解析的结果 */

    /* 解析参数 */
    if (parse_args(&cfg, &url, &parts, headers, argc, argv)) {
        usage();
        exit(1);
    }

    /* 拷贝解析结果的重要部分 */
    char *schema  = copy_url_part(url, &parts, UF_SCHEMA);
    char *host    = copy_url_part(url, &parts, UF_HOST);
    char *port    = copy_url_part(url, &parts, UF_PORT);
    char *service = port ? port : schema;

    /* 支持SSL */
    if (!strncmp("https", schema, 5)) {
        if ((cfg.ctx = ssl_init()) == NULL) {
            fprintf(stderr, "unable to initialize SSL\n");
            ERR_print_errors_fp(stderr);
            exit(1);
        }
        sock.connect  = ssl_connect;          /* 赋值插口的操控函数 */
        sock.close    = ssl_close;
        sock.read     = ssl_read;
        sock.write    = ssl_write;
        sock.readable = ssl_readable;
    }

    /* 设置信号处理，忽略 */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  SIG_IGN);

    /* 为统计结果分配内存 */
    statistics.latency  = stats_alloc(cfg.timeout * 1000);  /* 根据设定的请求超时时限(从发送请求到响应结束)，统计响应延迟信息 */
    statistics.requests = stats_alloc(MAX_THREAD_RATE_S);   /* 请求速率统计数组 */

    /* 分配线程信息结构 */
    thread *threads     = zcalloc(cfg.threads * sizeof(thread));

    /* 创建Lua虚拟机环境 */
    lua_State *L = script_create(cfg.script, url, headers);
    /* dns解析，结果存放在wrk.addrs */
    if (!script_resolve(L, host, service)) {
        char *msg = strerror(errno);
        fprintf(stderr, "unable to connect to %s:%s %s\n", host, service, msg);
        exit(1);
    }

    cfg.host = host;

    /* 启动多线程 */
    for (uint64_t i = 0; i < cfg.threads; i++) {
        thread *t      = &threads[i];
        t->loop        = aeCreateEventLoop(10 + cfg.connections * 3);  /* 创建事件驱动系统，如EPOLL */
        t->connections = cfg.connections / cfg.threads;

        t->L = script_create(cfg.script, url, headers);                /* 每个线程单独的Lua环境 */
        script_init(L, t, argc - optind, &argv[optind]);               /* 生成本线程发送的报文 */

        if (i == 0) {
            cfg.pipeline = script_verify_request(t->L);                /* 返回值为请求个数 */
            cfg.dynamic  = !script_is_static(t->L);                    /* 报文是否为动态生成，即是否注册了全局的request()函数 */
            cfg.delay    = script_has_delay(t->L);                     /* 是否配置了延迟函数delay() */
            if (script_want_response(t->L)) {                          /* 是否处理返回值，即是否注册全局的response()函数 */
                parser_settings.on_header_field = header_field;
                parser_settings.on_header_value = header_value;
                parser_settings.on_body         = response_body;       /* 注册解析回应报文的处理函数 */
            }
        }

        /* 启动线程 */
        if (!t->loop || pthread_create(&t->thread, NULL, &thread_main, t)) {
            char *msg = strerror(errno);
            fprintf(stderr, "unable to create thread %"PRIu64": %s\n", i, msg);
            exit(2);
        }
    }

    /* 设置中断信号处理句柄 */
    struct sigaction sa = {
        .sa_handler = handler,
        .sa_flags   = 0,
    };
    sigfillset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    /* 打印提示信息 */
    char *time = format_time_s(cfg.duration);
    printf("Running %s test @ %s\n", time, url);
    printf("  %"PRIu64" threads and %"PRIu64" connections\n", cfg.threads, cfg.connections);

    uint64_t start    = time_us();
    uint64_t complete = 0;
    uint64_t bytes    = 0;
    errors errors     = { 0 };

    /* 设定的运行周期结束后，结束线程 */
    sleep(cfg.duration);
    stop = 1;

    /* 等待工作线程结束 */
    for (uint64_t i = 0; i < cfg.threads; i++) {
        thread *t = &threads[i];
        pthread_join(t->thread, NULL);

        complete += t->complete;               /* 统计完成的请求数 */
        bytes    += t->bytes;                  /* 统计发送的字节数 */

        errors.connect += t->errors.connect;   /* 差错统计 */
        errors.read    += t->errors.read;
        errors.write   += t->errors.write;
        errors.timeout += t->errors.timeout;
        errors.status  += t->errors.status;
    }

    /* 输出统计信息 */
    uint64_t runtime_us = time_us() - start;
    long double runtime_s   = runtime_us / 1000000.0;
    long double req_per_s   = complete   / runtime_s;
    long double bytes_per_s = bytes      / runtime_s;

    if (complete / cfg.connections > 0) {
        int64_t interval = runtime_us / (complete / cfg.connections);   /* 计算理想的响应延迟 */
        stats_correct(statistics.latency, interval);
    }

    print_stats_header();
    print_stats("Latency", statistics.latency, format_time_us);
    print_stats("Req/Sec", statistics.requests, format_metric);
    if (cfg.latency) print_stats_latency(statistics.latency);

    char *runtime_msg = format_time_us(runtime_us);

    printf("  %"PRIu64" requests in %s, %sB read\n", complete, runtime_msg, format_binary(bytes));
    if (errors.connect || errors.read || errors.write || errors.timeout) {
        printf("  Socket errors: connect %d, read %d, write %d, timeout %d\n",
               errors.connect, errors.read, errors.write, errors.timeout);
    }

    if (errors.status) {
        printf("  Non-2xx or 3xx responses: %d\n", errors.status);
    }

    printf("Requests/sec: %9.2Lf\n", req_per_s);
    printf("Transfer/sec: %10sB\n", format_binary(bytes_per_s));

    /* 自定义统计结果输出 */
    if (script_has_done(L)) {
        script_summary(L, runtime_us, complete, bytes);           /* 设置表{duration, requests, bytes} */
        script_errors(L, &errors);                                /* 设置上表的{errors} */
        script_done(L, statistics.latency, statistics.requests);  /* 调用全局done() */
    }

    return 0;
}

/* 工作线程入口 */
void *thread_main(void *arg) {
    thread *thread = arg;

    char *request = NULL;
    size_t length = 0;

    /* 非动态生成报文，通过wrk.request()直接获取 */
    if (!cfg.dynamic) {
        script_request(thread->L, &request, &length);
    }

    /* 初始化连接信息结构 */
    thread->cs = zcalloc(thread->connections * sizeof(connection));
    connection *c = thread->cs;
    for (uint64_t i = 0; i < thread->connections; i++, c++) {
        c->thread = thread;
        c->ssl     = cfg.ctx ? SSL_new(cfg.ctx) : NULL;
        c->request = request;
        c->length  = length;
        c->delayed = cfg.delay;
        connect_socket(thread, c);
    }

    /* 100ms的定时器，统计请求速率 */
    aeEventLoop *loop = thread->loop;
    aeCreateTimeEvent(loop, RECORD_INTERVAL_MS, record_rate, thread, NULL);

    /* 启动 */
    thread->start = time_us();
    aeMain(loop);

    /* 资源清理 */
    aeDeleteEventLoop(loop);
    zfree(thread->cs);

    return NULL;
}

/* 连接服务器 */
static int connect_socket(thread *thread, connection *c) {
    struct addrinfo *addr = thread->addr;
    struct aeEventLoop *loop = thread->loop;
    int fd, flags;

    fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);

    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    /* 以下代码用于客户端绑定地址 */
    char client_addr[32] = {0};
    if (get_glb_str_from_lua(thread->L, "caddr", client_addr, sizeof(client_addr))) {
        #include <arpa/inet.h>
        struct sockaddr_in client_ip;
        
        int on=1;
        if((setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on))) < 0 ) {
            printf("setsockopt(SO_REUSEADDR) failed, %s\n", strerror(errno));
            goto error;
        }
        
        client_ip.sin_family = AF_INET;
        if(inet_pton(AF_INET, client_addr, &client_ip.sin_addr) <= 0) {
            printf("inet_pton() error, %s\n", strerror(errno));
            goto error;
        }
        client_ip.sin_port = 0;
        if(bind(fd, (struct sockaddr *)&client_ip, sizeof(client_ip)) == -1) {
            printf("bind() error, %s\n", strerror(errno));
            goto error;
        }
        
        usleep(random()%1000);            /* 随机睡眠，避免瞬间大连接 */
    }


    if (connect(fd, addr->ai_addr, addr->ai_addrlen) == -1) {
        if (errno != EINPROGRESS) goto error;
    }

    /* 设置非阻塞模式，方便使用epoll系统 */
    flags = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof(flags));

    /* 监控connect连接事件，处理句柄socket_connected() */
    flags = AE_READABLE | AE_WRITABLE;
    if (aeCreateFileEvent(loop, fd, flags, socket_connected, c) == AE_OK) {
        c->parser.data = c;
        c->fd = fd;
        return fd;
    }

  error:
    /* 连接错误 */
    thread->errors.connect++;
    close(fd);
    return -1;
}

/* 读差错后重新连接 */
static int reconnect_socket(thread *thread, connection *c) {
    aeDeleteFileEvent(thread->loop, c->fd, AE_WRITABLE | AE_READABLE);
    sock.close(c);
    close(c->fd);
    return connect_socket(thread, c);
}

/* 请求速率统计入口 */
static int record_rate(aeEventLoop *loop, long long id, void *data) {
    thread *thread = data;

    /* 统计 */
    if (thread->requests > 0) {
        uint64_t elapsed_ms = (time_us() - thread->start) / 1000;
        uint64_t requests = (thread->requests / (double) elapsed_ms) * 1000;  /* 有四舍五入的嫌疑 */

        /* 100ms内完成的请求平均数，做为请求速率的一个统计值 */
        stats_record(statistics.requests, requests);

        thread->requests = 0;
        thread->start    = time_us();
    }

    /* 设置事件循环系统的stop标识 */
    if (stop) aeStop(loop);

    return RECORD_INTERVAL_MS;
}

/* 延迟写的实现，只是去掉延迟标志，调用原函数socket_writeable() */
static int delay_request(aeEventLoop *loop, long long id, void *data) {
    connection *c = data;
    c->delayed = false;
    aeCreateFileEvent(loop, c->fd, AE_WRITABLE, socket_writeable, c);
    return AE_NOMORE;
}

static int header_field(http_parser *parser, const char *at, size_t len) {
    connection *c = parser->data;
    if (c->state == VALUE) {
        *c->headers.cursor++ = '\0';
        c->state = FIELD;
    }
    buffer_append(&c->headers, at, len);
    return 0;
}

static int header_value(http_parser *parser, const char *at, size_t len) {
    connection *c = parser->data;
    if (c->state == FIELD) {
        *c->headers.cursor++ = '\0';
        c->state = VALUE;
    }
    buffer_append(&c->headers, at, len);
    return 0;
}

static int response_body(http_parser *parser, const char *at, size_t len) {
    connection *c = parser->data;
    buffer_append(&c->body, at, len);
    return 0;
}

/* 响应数据处理完毕后，调用的回调函数 */
static int response_complete(http_parser *parser) {
    connection *c = parser->data;
    thread *thread = c->thread;
    uint64_t now = time_us();
    int status = parser->status_code;

    thread->complete++;     /* 本线程完成的请求数 */
    thread->requests++;     /* 本次速率统计期间，完成的请求统计 */

    if (status > 399) {     /* HTTP响应值错误统计 */
        thread->errors.status++;
    }

    if (c->headers.buffer) {/* 调用全局response() */
        *c->headers.cursor++ = '\0';
        script_response(thread->L, status, &c->headers, &c->body);
        c->state = FIELD;
    }

    if (--c->pending == 0) {/* 处理完毕 */
        if (!stats_record(statistics.latency, now - c->start)) {
            thread->errors.timeout++; /* 延时统计 */
        }
        c->delayed = cfg.delay;       /* 重新开启写事件 */
        aeCreateFileEvent(thread->loop, c->fd, AE_WRITABLE, socket_writeable, c);
    }

    /* 是否需要长连接 */
    if (!http_should_keep_alive(parser)) {
        reconnect_socket(thread, c);
        goto done;
    }

    /* 重新应答置位解析状态 */
    http_parser_init(parser, HTTP_RESPONSE);

  done:
    return 0;
}

/* 和服务器建立连接后的处理函数 */
static void socket_connected(aeEventLoop *loop, int fd, void *data, int mask) {
    connection *c = data;

    switch (sock.connect(c, cfg.host)) {
        case OK:    break;
        case ERROR: goto error;
        case RETRY: return;
    }

    /* 等待服务器端回应 */
    http_parser_init(&c->parser, HTTP_RESPONSE);
    c->written = 0;

    /* 读写事件 */
    aeCreateFileEvent(c->thread->loop, fd, AE_READABLE, socket_readable, c);
    aeCreateFileEvent(c->thread->loop, fd, AE_WRITABLE, socket_writeable, c);

    return;

  error:
    c->thread->errors.connect++;
    reconnect_socket(c->thread, c);
}

/* 写插口事件句柄 */
static void socket_writeable(aeEventLoop *loop, int fd, void *data, int mask) {
    connection *c = data;
    thread *thread = c->thread;

    /* 需要等待，则延迟发送 */
    if (c->delayed) {
        uint64_t delay = script_delay(thread->L);               /* 获取等待时间 */
        aeDeleteFileEvent(loop, fd, AE_WRITABLE);               /* 屏蔽写事件 */
        aeCreateTimeEvent(loop, delay, delay_request, c, NULL); /* 启动定时器，延迟写 */
        return;
    }

    /* 刚开始发送，生成报文 */
    if (!c->written) {
        if (cfg.dynamic) {                                      /* 动态生成，则调用全局的request() */
            script_request(thread->L, &c->request, &c->length);
        }
        c->start   = time_us();
        c->pending = cfg.pipeline;
    }

    char  *buf = c->request + c->written;
    size_t len = c->length  - c->written;
    size_t n;

    /* 发送报文，write() */
    switch (sock.write(c, buf, len, &n)) {
        case OK:    break;
        case ERROR: goto error;
        case RETRY: return;
    }

    /* 发送完毕后，删除继续发送事件，等待响应 */
    c->written += n;
    if (c->written == c->length) {
        c->written = 0;
        aeDeleteFileEvent(loop, fd, AE_WRITABLE);
    }

    return;

  error:
    /* 统计写错误，并重连服务器 */
    thread->errors.write++;
    reconnect_socket(thread, c);
}

/* 读插口事件句柄 */
static void socket_readable(aeEventLoop *loop, int fd, void *data, int mask) {
    connection *c = data;
    size_t n;

    do {
        switch (sock.read(c, &n)) {
            case OK:    break;
            case ERROR: goto error;
            case RETRY: return;
        }

        /* 解析报文 */
        if (http_parser_execute(&c->parser, &parser_settings, c->buf, n) != n) goto error;
        if (n == 0 && !http_body_is_final(&c->parser)) goto error;

        /* 增加接收报文字节数 */
        c->thread->bytes += n;
    } while (n == RECVBUF && sock.readable(c) > 0);

    return;

  error:
    /* 读差错统计 */
    c->thread->errors.read++;
    /* 重新连接 */
    reconnect_socket(c->thread, c);
}

static uint64_t time_us() {
    struct timeval t;
    gettimeofday(&t, NULL);
    return (t.tv_sec * 1000000) + t.tv_usec;
}

static char *copy_url_part(char *url, struct http_parser_url *parts, enum http_parser_url_fields field) {
    char *part = NULL;

    if (parts->field_set & (1 << field)) {
        uint16_t off = parts->field_data[field].off;
        uint16_t len = parts->field_data[field].len;
        part = zcalloc(len + 1 * sizeof(char));
        memcpy(part, &url[off], len);
    }

    return part;
}

static struct option longopts[] = {
    { "connections", required_argument, NULL, 'c' },
    { "duration",    required_argument, NULL, 'd' },
    { "threads",     required_argument, NULL, 't' },
    { "script",      required_argument, NULL, 's' },
    { "header",      required_argument, NULL, 'H' },
    { "latency",     no_argument,       NULL, 'L' },
    { "timeout",     required_argument, NULL, 'T' },
    { "help",        no_argument,       NULL, 'h' },
    { "version",     no_argument,       NULL, 'v' },
    { NULL,          0,                 NULL,  0  }
};

/* wrk命令行解析入口 */
static int parse_args(struct config *cfg, char **url, struct http_parser_url *parts, char **headers, int argc, char **argv) {
    char **header = headers;
    int c;

    /* 设置默认值 */
    memset(cfg, 0, sizeof(struct config));
    cfg->threads     = 2;                  /* 两个工作线程 */
    cfg->connections = 10;                 /* 默认链接数10 */
    cfg->duration    = 10;                 /* 工作时常10s */
    cfg->timeout     = SOCKET_TIMEOUT_MS;  /* 请求超时时限，默认2000ms */

    while ((c = getopt_long(argc, argv, "t:c:d:s:H:T:Lrv?", longopts, NULL)) != -1) {
        switch (c) {
        case 't':           /*-t, 指定工作线程数 */
            if (scan_metric(optarg, &cfg->threads)) return -1;
            break;
        case 'c':           /* 发起的连接数？？？ keepalive？？？ */
            if (scan_metric(optarg, &cfg->connections)) return -1;
            break;
        case 'd':           /* 程序运行时长，单位s */
            if (scan_time(optarg, &cfg->duration)) return -1;
            break;
        case 's':           /* 指定Lua脚本 */
            cfg->script = optarg;
            break;
        case 'H':           /* 指定的HTTP头部，格式"xxx: yyy" */
            *header++ = optarg;
            break;
        case 'L':           /* 打印延迟统计信息 */
            cfg->latency = true;
            break;
        case 'T':           /* 请求超时时限，单位s */
            if (scan_time(optarg, &cfg->timeout)) return -1;
            cfg->timeout *= 1000;
            break;
        case 'v':           /* 打印版本号 */
            printf("wrk %s [%s] ", VERSION, aeGetApiName());
            printf("Copyright (C) 2012 Will Glozer\n");
            break;
        case 'h':
        case '?':
        case ':':
        default:
            return -1;
        }
    }

    /* 检查参数，工作线程和工作时长不能为0 */
    if (optind == argc || !cfg->threads || !cfg->duration) return -1;

    /* 解析URL */
    if (!script_parse_url(argv[optind], parts)) {
        fprintf(stderr, "invalid URL: %s\n", argv[optind]);
        return -1;
    }

    /* 连接数必须大于工作线程数 */
    if (!cfg->connections || cfg->connections < cfg->threads) {
        fprintf(stderr, "number of connections must be >= threads\n");
        return -1;
    }

    /* 通过参数回传URL及参数中指定的HTTP头部字段 */
    *url    = argv[optind];
    *header = NULL;

    return 0;
}

static void print_stats_header() {
    printf("  Thread Stats%6s%11s%8s%12s\n", "Avg", "Stdev", "Max", "+/- Stdev");
}

static void print_units(long double n, char *(*fmt)(long double), int width) {
    char *msg = fmt(n);
    int len = strlen(msg), pad = 2;

    if (isalpha(msg[len-1])) pad--;
    if (isalpha(msg[len-2])) pad--;
    width -= pad;

    printf("%*.*s%.*s", width, width, msg, pad, "  ");

    free(msg);
}

static void print_stats(char *name, stats *stats, char *(*fmt)(long double)) {
    uint64_t max = stats->max;
    long double mean  = stats_mean(stats);
    long double stdev = stats_stdev(stats, mean);

    printf("    %-10s", name);
    print_units(mean,  fmt, 8);
    print_units(stdev, fmt, 10);
    print_units(max,   fmt, 9);
    printf("%8.2Lf%%\n", stats_within_stdev(stats, mean, stdev, 1));
}

static void print_stats_latency(stats *stats) {
    long double percentiles[] = { 50.0, 75.0, 90.0, 99.0 };
    printf("  Latency Distribution\n");
    for (size_t i = 0; i < sizeof(percentiles) / sizeof(long double); i++) {
        long double p = percentiles[i];
        uint64_t n = stats_percentile(stats, p);
        printf("%7.0Lf%%", p);
        print_units(n, format_time_us, 10);
        printf("\n");
    }
}
