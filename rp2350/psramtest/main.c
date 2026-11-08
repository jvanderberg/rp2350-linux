/* PSRAM bring-up test for an RP2350B with PSRAM on QMI CS1 (GP0).
 *
 * QMI setup follows PicoMite/FanPico psram.c (Timo Kokkonen, GPL-3.0):
 * exit QPI, read ID, reset, enter QPI, then program M1 timing/rfmt/wfmt.
 *
 * Output goes to RTT (probe-rs run picks it up). Results are also left in
 * the global `result` so they can be read back over SWD if RTT fails.
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/vreg.h"

/* Target clock; 0 = leave the boot default (150 MHz). */
#ifndef SYS_KHZ
#define SYS_KHZ 0
#endif
#ifndef VREG
#define VREG VREG_VOLTAGE_1_20
#endif
#ifndef VREG_MV
#define VREG_MV 1200
#endif
#ifndef PASSES
#define PASSES 1
#endif

#define PSRAM_CS_PIN 0
#define PSRAM_BASE 0x11000000u
#define PSRAM_NOCACHE_BASE 0x15000000u
#define PSRAM_WINDOW (16u << 20)
#define PSRAM_MAX_CSR_CLK 5000000u

#define CMD_QUAD_READ 0xeb
#define CMD_QUAD_WRITE 0x38
#define CMD_ENTER_QPI 0x35
#define CMD_EXIT_QPI 0xf5
#define CMD_RESET_ENABLE 0x66
#define CMD_RESET 0x99
#define CMD_READ_ID 0x9f

#define KGD_PASS 0x5d

struct result {
    uint32_t magic; /* 'PSRT' once the test has finished */
    uint32_t stage;
    uint8_t id[8]; /* mfid, kgd, eid[6] */
    uint32_t size;
    uint32_t errors;
    uint32_t first_bad_addr, first_bad_got, first_bad_want;
};
volatile struct result result;

static inline void csr_busy_wait(void)
{
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS)
        tight_loop_contents();
}

static inline void csr_txempty_wait(void)
{
    while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS))
        tight_loop_contents();
}

static void __no_inline_not_in_flash_func(csr_cmd)(uint32_t cmd)
{
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS1N_BITS);
    qmi_hw->direct_tx = cmd;
    csr_txempty_wait();
    csr_busy_wait();
    (void)qmi_hw->direct_rx;
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS1N_BITS);
}

static void __no_inline_not_in_flash_func(psram_read_id)(uint8_t csr_clkdiv, uint8_t *buf)
{
    uint32_t irq = save_and_disable_interrupts();
    qmi_hw->direct_csr = csr_clkdiv << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
    csr_busy_wait();
    csr_cmd(QMI_DIRECT_TX_OE_BITS | QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB |
            CMD_EXIT_QPI);
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS1N_BITS);
    for (int i = 0; i < 12; i++) {
        qmi_hw->direct_tx = i == 0 ? CMD_READ_ID : 0;
        csr_txempty_wait();
        csr_busy_wait();
        uint8_t rx = qmi_hw->direct_rx;
        if (i >= 4)
            buf[i - 4] = rx;
    }
    hw_clear_bits(&qmi_hw->direct_csr,
                  QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_CS1N_BITS);
    restore_interrupts(irq);
}

static void __no_inline_not_in_flash_func(psram_qmi_setup)(uint8_t clkdiv, uint8_t csr_clkdiv,
                                                           uint8_t max_select,
                                                           uint8_t min_deselect,
                                                           uint8_t rxdelay)
{
    uint32_t irq = save_and_disable_interrupts();
    qmi_hw->direct_csr = csr_clkdiv << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
    csr_busy_wait();
    csr_cmd(CMD_RESET_ENABLE);
    csr_cmd(CMD_RESET);
    csr_cmd(CMD_ENTER_QPI);
    hw_clear_bits(&qmi_hw->direct_csr,
                  QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_CS1N_BITS);

    qmi_hw->m[1].timing = 1 << QMI_M1_TIMING_COOLDOWN_LSB |
                          QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
                          0 << QMI_M1_TIMING_SELECT_SETUP_LSB |
                          3 << QMI_M1_TIMING_SELECT_HOLD_LSB |
                          max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
                          min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
                          rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
                          clkdiv << QMI_M1_TIMING_CLKDIV_LSB;
    qmi_hw->m[1].rfmt = QMI_M1_RFMT_DUMMY_LEN_VALUE_24 << QMI_M1_RFMT_DUMMY_LEN_LSB |
                        QMI_M1_RFMT_SUFFIX_LEN_VALUE_NONE << QMI_M1_RFMT_SUFFIX_LEN_LSB |
                        QMI_M1_RFMT_PREFIX_LEN_VALUE_8 << QMI_M1_RFMT_PREFIX_LEN_LSB |
                        QMI_M1_RFMT_DATA_WIDTH_VALUE_Q << QMI_M1_RFMT_DATA_WIDTH_LSB |
                        QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_RFMT_DUMMY_WIDTH_LSB |
                        QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_SUFFIX_WIDTH_LSB |
                        QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_RFMT_ADDR_WIDTH_LSB |
                        QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_PREFIX_WIDTH_LSB;
    qmi_hw->m[1].rcmd = CMD_QUAD_READ << QMI_M1_RCMD_PREFIX_LSB;
    qmi_hw->m[1].wfmt = QMI_M1_WFMT_DUMMY_LEN_VALUE_NONE << QMI_M1_WFMT_DUMMY_LEN_LSB |
                        QMI_M1_WFMT_SUFFIX_LEN_VALUE_NONE << QMI_M1_WFMT_SUFFIX_LEN_LSB |
                        QMI_M1_WFMT_PREFIX_LEN_VALUE_8 << QMI_M1_WFMT_PREFIX_LEN_LSB |
                        QMI_M1_WFMT_DATA_WIDTH_VALUE_Q << QMI_M1_WFMT_DATA_WIDTH_LSB |
                        QMI_M1_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_WFMT_DUMMY_WIDTH_LSB |
                        QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_SUFFIX_WIDTH_LSB |
                        QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_WFMT_ADDR_WIDTH_LSB |
                        QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_PREFIX_WIDTH_LSB;
    qmi_hw->m[1].wcmd = CMD_QUAD_WRITE << QMI_M1_WCMD_PREFIX_LSB;
    restore_interrupts(irq);

    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);
}

/* Smallest power of two at which the address space wraps. */
static uint32_t detect_size(void)
{
    volatile uint32_t *p = (volatile uint32_t *)PSRAM_NOCACHE_BASE;
    for (uint32_t sz = 1u << 20; sz < PSRAM_WINDOW; sz <<= 1) {
        p[0] = 0;
        p[sz / 4] = 0xc0ffee42;
        if (p[0] == 0xc0ffee42) {
            p[0] = 0;
            return sz;
        }
        p[sz / 4] = 0;
    }
    return PSRAM_WINDOW;
}

static inline uint32_t pattern(uint32_t i, uint32_t seed)
{
    uint32_t x = i * 0x9e3779b9u ^ seed;
    x ^= x >> 15;
    return x * 0x2c1b3c6du;
}

static void note_error(uint32_t addr, uint32_t got, uint32_t want)
{
    if (result.errors++ == 0) {
        result.first_bad_addr = addr;
        result.first_bad_got = got;
        result.first_bad_want = want;
    }
}

/* Write through `wbase`, verify through `rbase`. */
static void memtest(uint32_t wbase, uint32_t rbase, uint32_t size, uint32_t seed)
{
    volatile uint32_t *w = (volatile uint32_t *)wbase;
    volatile uint32_t *r = (volatile uint32_t *)rbase;
    uint32_t n = size / 4;
    uint64_t t0 = time_us_64();
    for (uint32_t i = 0; i < n; i++)
        w[i] = pattern(i, seed);
    uint64_t t1 = time_us_64();
    uint32_t before = result.errors;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t got = r[i], want = pattern(i, seed);
        if (got != want)
            note_error(rbase + i * 4, got, want);
    }
    uint64_t t2 = time_us_64();
    printf("  write %08lx.. %lu ms (%lu KB/s), read %08lx.. %lu ms (%lu KB/s), errors %lu\n",
           (unsigned long)wbase, (unsigned long)((t1 - t0) / 1000),
           (unsigned long)((uint64_t)size * 1000 / (t1 - t0 + 1)), (unsigned long)rbase,
           (unsigned long)((t2 - t1) / 1000),
           (unsigned long)((uint64_t)size * 1000 / (t2 - t1 + 1)),
           (unsigned long)(result.errors - before));
}

/* Average ns per dependent random 32-bit load over `range` bytes. */
static uint32_t random_read_ns(uint32_t base, uint32_t range, uint32_t count)
{
    volatile uint32_t *p = (volatile uint32_t *)base;
    uint32_t mask = range / 4 - 1, x = 12345, sink = 0;
    uint64_t t0 = time_us_64();
    for (uint32_t i = 0; i < count; i++) {
        x = x * 1664525u + 1013904223u + sink;
        sink = p[(x >> 8) & mask] & 0; /* data-dependent so loads serialise */
    }
    uint64_t t1 = time_us_64();
    return (uint32_t)((t1 - t0) * 1000 / count);
}

/* Flash (M0) timing that is in spec at both 150 and 300 MHz:
 * COOLDOWN=1, RXDELAY=2, MIN_DESELECT=6, CLKDIV=4. Done from SRAM so we are
 * not fetching from flash while its timing changes. */
static void __no_inline_not_in_flash_func(flash_slow_timing)(void)
{
    qmi_hw->m[0].timing = 0x40006204;
    for (volatile int i = 0; i < 1000; i++)
        ;
}

/* Core clock measured against the 1 MHz timer with the DWT cycle counter. */
#define DEMCR (*(volatile uint32_t *)0xE000EDFC)
#define DWT_CTRL (*(volatile uint32_t *)0xE0001000)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004)
static uint32_t measure_core_khz(void)
{
    DEMCR |= 1u << 24;
    DWT_CTRL |= 1;
    uint64_t t0 = time_us_64();
    while (time_us_64() == t0)
        ;
    uint32_t c0 = DWT_CYCCNT;
    t0 = time_us_64();
    while (time_us_64() - t0 < 100000)
        ;
    uint32_t c1 = DWT_CYCCNT;
    return (c1 - c0) / 100;
}

int main(void)
{
    stdio_init_all();
    sleep_ms(200);
    if (SYS_KHZ) {
        vreg_set_voltage(VREG);
        sleep_ms(10);
        flash_slow_timing();
        bool ok = set_sys_clock_khz(SYS_KHZ, false);
        flash_slow_timing(); /* clock change can reset pads/timing */
        printf("\nset_sys_clock_khz(%u) -> %s, vreg %u mV\n", SYS_KHZ, ok ? "ok" : "FAILED",
               VREG_MV);
    }
    uint32_t sys_clk = clock_get_hz(clk_sys);
    printf("\n=== psramtest: RP2350, sys_clk %lu Hz, PSRAM CS1 on GP%d ===\n",
           (unsigned long)sys_clk, PSRAM_CS_PIN);
    printf("measured core clock: %lu kHz (DWT cycles vs 1 MHz timer)\n",
           (unsigned long)measure_core_khz());

    result.stage = 1;
    gpio_set_function(PSRAM_CS_PIN, GPIO_FUNC_XIP_CS1);
    uint8_t csr_clkdiv = (sys_clk + PSRAM_MAX_CSR_CLK - 1) / PSRAM_MAX_CSR_CLK;
    uint8_t id[8];
    psram_read_id(csr_clkdiv, id);
    for (int i = 0; i < 8; i++)
        result.id[i] = id[i];
    printf("ID: mfid %02x kgd %02x eid %02x %02x %02x %02x %02x %02x\n", id[0], id[1], id[2],
           id[3], id[4], id[5], id[6], id[7]);
    if (id[1] != KGD_PASS) {
        printf("FAIL: no PSRAM (KGD %02x, want %02x)\n", id[1], KGD_PASS);
        result.magic = 0x54525350;
        for (;;)
            tight_loop_contents();
    }
    const char *mf = id[0] == 0x0d ? "AP Memory" : id[0] == 0x9d ? "ISSI" : "unknown";
    uint32_t id_mb = 0;
    if (id[0] == 0x0d)
        id_mb = (id[2] >> 5) == 2 ? 8 : (id[2] >> 5) == 1 ? 4 : 2;
    printf("vendor %s, ID says %lu MB\n", mf, (unsigned long)id_mb);

    /* AP Memory APS6404L limits: 109 MHz, tCEM 8 us. */
    result.stage = 2;
    uint32_t period_fs = (uint32_t)(1000000000000000ull / sys_clk);
    uint8_t clkdiv = (sys_clk + 109000000 - 1) / 109000000;
    uint8_t max_select = (uint8_t)((8000000000ull >> 6) / period_fs);
    uint8_t min_deselect = (uint8_t)((18000000u + period_fs - 1) / period_fs);
    uint8_t rxdelay = 1 + (sys_clk > 150000000 ? clkdiv : 1);
#ifdef PSRAM_CLKDIV /* force a divider, e.g. to run the PSRAM above its rating */
    clkdiv = PSRAM_CLKDIV;
    rxdelay = 1 + (sys_clk > 150000000 ? clkdiv : 1);
#endif
#ifdef PSRAM_RXDELAY
    rxdelay = PSRAM_RXDELAY;
#endif
    psram_qmi_setup(clkdiv, csr_clkdiv, max_select, min_deselect, rxdelay);
    printf("QMI M1: clkdiv %u (%lu MHz SCK), max_select %u, min_deselect %u, rxdelay %u\n",
           clkdiv, (unsigned long)(sys_clk / clkdiv / 1000000), max_select, min_deselect,
           rxdelay);

    result.stage = 3;
    uint32_t size = detect_size();
    result.size = size;
    printf("detected size: %lu KB\n", (unsigned long)(size >> 10));

    result.stage = 4;
    uint64_t soak_t0 = time_us_64();
    int rounds = 0;
    for (int pass = 0;; pass++) {
#ifdef SOAK_S
    if (time_us_64() - soak_t0 >= (uint64_t)SOAK_S * 1000000)
        break;
    printf("-- round %d, t=%lu s, errors so far %lu\n", pass + 1,
           (unsigned long)((time_us_64() - soak_t0) / 1000000), (unsigned long)result.errors);
#else
    if (pass >= PASSES)
        break;
    printf("-- round %d/%d\n", pass + 1, PASSES);
#endif
    rounds++;
    printf("pass 1: uncached write, uncached read\n");
    memtest(PSRAM_NOCACHE_BASE, PSRAM_NOCACHE_BASE, size, 0x1234567 + (uint32_t)pass);
    printf("pass 2: uncached write, cached read\n");
    memtest(PSRAM_NOCACHE_BASE, PSRAM_BASE, size, 0x89abcdef - (uint32_t)pass);
    printf("pass 3: cached write, cached read (write-back through XIP cache)\n");
    memtest(PSRAM_BASE, PSRAM_BASE, size, 0x5a5a5a5a ^ (uint32_t)pass);
    }

    result.stage = 5;
    printf("random dependent 32-bit loads:\n");
    printf("  uncached, 8 MB range:  %lu ns\n",
           (unsigned long)random_read_ns(PSRAM_NOCACHE_BASE, size, 200000));
    printf("  cached,   8 MB range:  %lu ns\n",
           (unsigned long)random_read_ns(PSRAM_BASE, size, 200000));
    printf("  cached,   4 KB range:  %lu ns\n",
           (unsigned long)random_read_ns(PSRAM_BASE, 4096, 200000));

    printf("RESULT: %s, %lu MB, %lu errors, %d rounds (%lu MB verified) in %lu s\n",
           result.errors ? "FAIL" : "PASS", (unsigned long)(size >> 20),
           (unsigned long)result.errors, rounds, (unsigned long)(rounds * 3 * (size >> 20)),
           (unsigned long)((time_us_64() - soak_t0) / 1000000));
    if (result.errors)
        printf("  first bad @%08lx got %08lx want %08lx\n",
               (unsigned long)result.first_bad_addr, (unsigned long)result.first_bad_got,
               (unsigned long)result.first_bad_want);
    result.magic = 0x54525350;
    for (;;)
        sleep_ms(1000);
}
