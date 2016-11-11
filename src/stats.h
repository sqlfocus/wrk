#ifndef STATS_H
#define STATS_H

#include <stdbool.h>
#include <stdint.h>

#define MAX(X, Y) ((X) > (Y) ? (X) : (Y))
#define MIN(X, Y) ((X) < (Y) ? (X) : (Y))

typedef struct {
    uint32_t connect;
    uint32_t read;
    uint32_t write;
    uint32_t status;
    uint32_t timeout;
} errors;

/* 统计信息结构 */
typedef struct {
    uint64_t count;
    uint64_t limit;       /* data[]数组长度 */
    uint64_t min;
    uint64_t max;
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
