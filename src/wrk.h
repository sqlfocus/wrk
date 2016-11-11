#ifndef WRK_H
#define WRK_H

#include "config.h"
#include <pthread.h>
#include <inttypes.h>
#include <sys/types.h>
#include <netdb.h>
#include <sys/socket.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <luajit-2.0/lua.h>

#include "stats.h"
#include "ae.h"
#include "http_parser.h"

#define RECVBUF  8192

#define MAX_THREAD_RATE_S   10000000     /* 最大的请求速率，个/s??? */
#define SOCKET_TIMEOUT_MS   2000         /* 默认的请求超时时限，ms */
#define RECORD_INTERVAL_MS  100          /**/

extern const char *VERSION;

/* 内部的线程信息结构 */
typedef struct {
    pthread_t thread;
    aeEventLoop *loop;           /* 事件驱动系统信息，如linux的EPOLL */
    struct addrinfo *addr;       /* 发起连接的目的地址；根据域名DNS而来，选取可连接的第一个 */
    uint64_t connections;        /* = cfg.connections / cfg.threads */
    uint64_t complete;
    uint64_t requests;
    uint64_t bytes;              /* 接收到的字节数 */
    uint64_t start;              /* 开始时间，单位us */
    lua_State *L;
    errors errors;
    struct connection *cs;       /* 维护连接信息，个数->connections */
} thread;

typedef struct {
    char  *buffer;
    size_t length;
    char  *cursor;
} buffer;

/* 维护线程的连接信息 */
typedef struct connection {
    thread *thread;             /* 回指对应的线程信息结构 */
    http_parser parser;         /* 监控connect事件时，->data指向本结构自身 */
    enum {
        FIELD, VALUE
    } state;
    int fd;                     /* 本地插口 */
    SSL *ssl;
    bool delayed;               /* 是否需要执行delay() */
    uint64_t start;             /* 开始时间，us */
    char *request;              /* 请求报文内存首地址 */
    size_t length;              /* 请求报文长度 */
    size_t written;             /* 已发送的长度 */
    uint64_t pending;           /* 待发送报文个数 */
    buffer headers;
    buffer body;
    char buf[RECVBUF];
} connection;

#endif /* WRK_H */
