#ifndef STATS_H
#define STATS_H

#include <stdbool.h>
#include <stdint.h>

#define MAX(X, Y) ((X) > (Y) ? (X) : (Y))
#define MIN(X, Y) ((X) < (Y) ? (X) : (Y))

/* 差错统计信息 */
typedef struct {
    uint32_t connect;     /* 连接错误，connect() */
    uint32_t read;        /* 读错误, read() */
    uint32_t write;       /* 写错误, write() */
    uint32_t status;      /* 非200 + 300的HTTP回应报文计数 */
    uint32_t timeout;     /* 响应超时统计 */
} errors;

/* 统计信息结构 */
typedef struct {
    uint64_t count;       /* 统计总次数 */
    uint64_t limit;       /* data[]数组长度 */
    uint64_t min;         /* 索引的最小值，方便计算方差或统计值 */
    uint64_t max;         /* 索引的最大值 */
    uint64_t data[];      /* 统计信息边长数组 */
} stats;

stats *stats_alloc(uint64_t);
void stats_free(stats *);

int stats_record(stats *, uint64_t);
void stats_correct(stats *, int64_t);

long double stats_mean(stats *);
long double stats_stdev(stats *stats, long double);
long double stats_within_stdev(stats *, long double, long double, uint64_t);
uint64_t stats_percentile(stats *, long double);

uint64_t stats_popcount(stats *);
uint64_t stats_value_at(stats *stats, uint64_t, uint64_t *);

#endif /* STATS_H */
