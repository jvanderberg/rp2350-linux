/*
 * pulse.h - state shared between pulse's sampler (pulse_data.c) and its
 * user interface (pulse.c). Split in two so tcc fits in the board's RAM.
 */
#ifndef PULSE_H
#define PULSE_H

#include <stddef.h>
#include <sys/utsname.h>

#define HIST    512             /* history samples kept for graphs */
#define MAXPROC 512
#define MAXCPU  8               /* cores tracked individually */

typedef struct {
    int pid, ppid;
    char name[24];
    char state;
    int nice;
    int threads;
    unsigned long rss_kb;
    unsigned long vsize_kb;
    unsigned long ticks;        /* CPU time, user and system */
    unsigned long start;        /* clock ticks after boot */
    int cpu_pm;                 /* permille of one core (can exceed 1000) */
} Proc;

enum { SORT_CPU, SORT_MEM, SORT_PID, SORT_NAME, SORT_COUNT,
       /* more, for xpulse's columns; pulse's s key cycles the first four */
       SORT_PPID = SORT_COUNT, SORT_STATE, SORT_NICE, SORT_THR, SORT_VIRT, SORT_TIME };

extern struct utsname uts;
extern int ncpu;

extern int cpu_pm, usr_pm, sys_pm, irq_pm, io_pm;      /* permille */
extern int core_pm[MAXCPU];     /* per-core busy, permille */
extern int ncore;               /* cores in core_pm/h_core (<= MAXCPU) */
extern unsigned long ctxt_rate, intr_rate;
extern char load1[8], load5[8], load15[8];
extern int tasks_running, tasks_total;
extern unsigned long uptime_s;

extern unsigned long mem_total, mem_free, mem_avail, mem_buffers, mem_cached;
extern unsigned long mem_used;
extern int mem_used_pm;

extern char net_name[24];
extern unsigned long long rx_cur, tx_cur;
extern unsigned long rx_rate, tx_rate;

extern char disk_name[32];
extern unsigned long rd_rate, wr_rate;
extern int disk_busy_pm;
extern unsigned long long fs_total, fs_used;

/* Big tables are heap-allocated by pulse_init(): as static arrays they
 * push tcc past this board's memory limit at compile time. */
extern unsigned int *h_cpu, *h_mem, *h_rx, *h_tx, *h_disk;  /* HIST each */
extern unsigned int *h_core;    /* MAXCPU * HIST; core i at h_core + i * HIST */
extern int h_count;

extern Proc *procs;             /* MAXPROC entries */
extern int nprocs;
extern long page_kb;
extern int sort_key, sort_rev, sel, sel_pid;

int pulse_init(void);           /* 0 on success, -1 if out of memory */
void sample(void);
void sort_procs(void);
void fmt_bytes(char *out, size_t n, unsigned long long b);

#endif /* PULSE_H */
