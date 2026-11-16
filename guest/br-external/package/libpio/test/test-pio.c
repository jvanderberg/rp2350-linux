/* libpio against the fake device (fakedev.c): what pio.c writes into the
 * registers and asks of the driver, without a board.
 *
 *   make -C guest/br-external/package/libpio/test test
 */
#include "fakedev.h"
#include "pio.h"
#include "rvemu-pio.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond, ...)                                                \
    do {                                                                \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            fails++;                                                    \
            printf("FAIL %s:%d: %s: ", __func__, __LINE__, #cond);      \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                               \
        }                                                               \
    } while (0)

/* PINCTRL / EXECCTRL fields */
#define OUT_BASE(p) ((p) & 31)
#define SET_BASE(p) ((p) >> 5 & 31)
#define SIDE_BASE(p) ((p) >> 10 & 31)
#define IN_BASE(p) ((p) >> 15 & 31)
#define JMP_PIN(e) ((e) >> 24 & 31)

static PIO fresh(void)
{
    fake_reset();
    PIO pio = pio_open(1);
    if (!pio) {
        perror("pio_open on the fake");
        return NULL;
    }
    return pio;
}

static pio_sm_config pins_at(uint out, uint set, uint side, uint in, uint jmp)
{
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_out_pins(&c, out, 1);
    sm_config_set_set_pins(&c, set, 1);
    sm_config_set_sideset(&c, 1, false, false);
    sm_config_set_sideset_pins(&c, side);
    sm_config_set_in_pins(&c, in);
    sm_config_set_jmp_pin(&c, jmp);
    return c;
}

/* GPIO numbers in, GPIOBASE-relative indices in the registers: the
 * documentation pass found pins above 31 landing 16 pins too low */
static void test_pins_low_base(void)
{
    PIO pio = fresh();
    pio_sm_config c = pins_at(1, 2, 3, 13, 14);
    CHECK(pio_sm_set_config(pio, 0, &c) == 0, "errno %d", errno);
    CHECK(pio_get_gpio_base(pio) == 0, "base %u", pio_get_gpio_base(pio));
    uint32_t p = pio->sm[0].pinctrl, e = pio->sm[0].execctrl;
    CHECK(OUT_BASE(p) == 1 && SET_BASE(p) == 2 && SIDE_BASE(p) == 3 && IN_BASE(p) == 13 && JMP_PIN(e) == 14,
          "out %u set %u side %u in %u jmp %u", OUT_BASE(p), SET_BASE(p), SIDE_BASE(p), IN_BASE(p), JMP_PIN(e));
    pio_close(pio);
}

static void test_pins_high_base(void)
{
    PIO pio = fresh();
    pio_sm_config c = pins_at(33, 47, 16, 20, 35);
    CHECK(pio_sm_set_config(pio, 1, &c) == 0, "errno %d", errno);
    CHECK(pio_get_gpio_base(pio) == 16, "base %u", pio_get_gpio_base(pio));
    uint32_t p = pio->sm[1].pinctrl, e = pio->sm[1].execctrl;
    /* GP33 is index 17 from GPIOBASE 16, GP47 31, GP16 0, GP20 4, GP35 19 */
    CHECK(OUT_BASE(p) == 17 && SET_BASE(p) == 31 && SIDE_BASE(p) == 0 && IN_BASE(p) == 4 && JMP_PIN(e) == 19,
          "out %u set %u side %u in %u jmp %u", OUT_BASE(p), SET_BASE(p), SIDE_BASE(p), IN_BASE(p), JMP_PIN(e));
    pio_close(pio);
}

static void test_direct_setters_follow_base(void)
{
    PIO pio = fresh();
    pio_set_gpio_base(pio, 16);
    pio_sm_set_out_pins(pio, 2, 40, 2);
    pio_sm_set_set_pins(pio, 2, 41, 1);
    pio_sm_set_in_pins(pio, 2, 42);
    pio_sm_set_sideset_pins(pio, 2, 43);
    pio_sm_set_jmp_pin(pio, 2, 44);
    uint32_t p = pio->sm[2].pinctrl, e = pio->sm[2].execctrl;
    CHECK(OUT_BASE(p) == 24 && SET_BASE(p) == 25 && IN_BASE(p) == 26 && SIDE_BASE(p) == 27 && JMP_PIN(e) == 28,
          "out %u set %u in %u side %u jmp %u", OUT_BASE(p), SET_BASE(p), IN_BASE(p), SIDE_BASE(p), JMP_PIN(e));
    pio_set_gpio_base(pio, 0);
    pio_sm_set_jmp_pin(pio, 2, 7);
    CHECK(JMP_PIN(pio->sm[2].execctrl) == 7, "jmp %u", JMP_PIN(pio->sm[2].execctrl));
    pio_close(pio);
}

/* pins no one base reaches; and a base switch that would move another
 * machine's pins */
static void test_base_conflicts(void)
{
    PIO pio = fresh();
    pio_sm_config c = pins_at(3, 3, 3, 3, 40);
    CHECK(pio_sm_set_config(pio, 0, &c) < 0 && errno == EINVAL, "GP3 with GP40 accepted");

    pio_sm_config lo = pins_at(5, 5, 5, 5, 5), hi = pins_at(40, 40, 40, 40, 40);
    CHECK(pio_sm_set_config(pio, 0, &lo) == 0, "errno %d", errno);
    /* sm 0 is set up for base 0 (not running): sm 1 can't move the base */
    CHECK(pio_sm_set_config(pio, 1, &hi) < 0 && errno == EINVAL, "base switched under sm 0's pins");
    CHECK(pio_get_gpio_base(pio) == 0, "base %u", pio_get_gpio_base(pio));
    /* sm 0 itself may move it */
    CHECK(pio_sm_set_config(pio, 0, &hi) == 0 && pio_get_gpio_base(pio) == 16, "errno %d", errno);
    /* a running machine pins the base too */
    pio_sm_set_enabled(pio, 0, true);
    CHECK(pio_sm_set_config(pio, 1, &lo) < 0 && errno == EINVAL, "base switched under a running machine");
    pio_close(pio);
}

/* GPIO numbers in the 48-pin masks, whatever the base */
static void test_pin_masks(void)
{
    PIO pio = fresh();
    pio_set_gpio_base(pio, 16);
    pio->sm[0].pinctrl = 0x12345;
    /* GP3 is out of reach from base 16: nothing runs */
    pio_sm_set_pindirs_with_mask(pio, 0, 1ull << 3, 1ull << 3);
    CHECK(pio->sm[0].instr == 0, "ran %04x for GP3", (unsigned)pio->sm[0].instr);
    /* GP33: one SET pindirs, 1, and PINCTRL put back after. (The fake keeps
     * no register history, so the SET_BASE used for it, index 17, is checked
     * on a real block by guest/tests/pio, not here.) */
    pio_sm_set_pindirs_with_mask(pio, 0, 1ull << 33, 1ull << 33);
    CHECK(pio->sm[0].instr == pio_encode_set(pio_pindirs, 1), "instr %04x", (unsigned)pio->sm[0].instr);
    CHECK(pio->sm[0].pinctrl == 0x12345, "PINCTRL not restored: %08x", (unsigned)pio->sm[0].pinctrl);
    pio_close(pio);
}

static void test_pin_claims(void)
{
    PIO pio = fresh();
    CHECK(pio_gpio_init(pio, 33) == 0, "errno %d", errno);
    CHECK(fake_count(RVEMU_PIO_PIN_CLAIM, 33) == 1, "GP33 not claimed");
    fake.busy = 1ull << 14;
    CHECK(pio_gpio_init(pio, 14) < 0 && errno == EBUSY, "a busy pin's claim succeeded");
    pio_close(pio);
}

/* the review's finding: a failed restart lost the channel, so it was never
 * stopped and stayed taken until close */
static void test_dma_copy_gives_channel_back(void)
{
    static uint32_t data[6000];
    PIO pio = fresh();
    CHECK(pio_dma_buffer(pio, 16384) != NULL, "errno %d", errno);
    fake.start_fails_at = 2; /* 24000 bytes: two chunks, the second refused */
    CHECK(pio_sm_dma_write(pio, 0, data, sizeof(data), 4) < 0, "a failed transfer succeeded");
    CHECK(fake_count(RVEMU_PIO_DMA_STOP, 0) == 1, "channel 0 stopped %d times", fake_count(RVEMU_PIO_DMA_STOP, 0));
    CHECK(fake_dma_owned() == 0, "%d channels still taken", fake_dma_owned());

    fake.start_fails_at = 0;
    fake.starts = 0;
    CHECK(pio_sm_dma_read(pio, 1, data, sizeof(data), 4) == 0, "errno %d", errno);
    CHECK(fake_dma_owned() == 0, "%d channels still taken", fake_dma_owned());
    pio_close(pio);
}

/* programs: JMPs relocated, and pio_encode_* the same as the assembler */
static void test_program_loading(void)
{
    char err[128];
    pio_asm_program_t a;
    PIO pio = fresh();
    int r = pio_assemble(".program p\ntop:\n  set x, 3\nloop:\n  jmp x-- loop\n  jmp top\n", "p", &a, err, sizeof(err));
    CHECK(r == 0, "%s", err);
    CHECK(pio_add_program_at_offset(pio, &a.program, 20) == 20, "errno %d", errno);
    CHECK(pio->instr_mem[20] == pio_encode_set(pio_x, 3), "%04x", (unsigned)pio->instr_mem[20]);
    CHECK(pio->instr_mem[21] == pio_encode_jmp_x_dec(21), "%04x", (unsigned)pio->instr_mem[21]);
    CHECK(pio->instr_mem[22] == pio_encode_jmp(20), "%04x", (unsigned)pio->instr_mem[22]);
    /* overlapping it is refused */
    CHECK(pio_add_program_at_offset(pio, &a.program, 21) < 0, "overlap accepted");
    pio_close(pio);

    static const struct {
        const char *src;
        unsigned word;
    } enc[] = {
        { "nop", 0xa042 },
        { "set pindirs, 1", 0xe081 },
        { "set y, 31", 0xe05f },
        { "jmp pin, 0", 0x00c0 },
        { "jmp !osre, 7", 0x00e7 },
        { "wait 1 pin, 4", 0x20a4 },
        { "wait 0 irq, 3 rel", 0x2053 },
        { "in pins, 8", 0x4008 },
        { "in isr, 32", 0x40c0 },
        { "out pindirs, 2", 0x6082 },
        { "out exec, 16", 0x60f0 },
        { "push block", 0x8020 },
        { "pull noblock", 0x8080 },
        { "mov x, ~y", 0xa02a },
        { "mov pins, ::isr", 0xa016 },
        { "irq clear 5", 0xc045 },
        { "irq wait 2 rel", 0xc032 },
    };
    unsigned want[] = {
        pio_encode_nop(),
        pio_encode_set(pio_pindirs, 1),
        pio_encode_set(pio_y, 31),
        pio_encode_jmp_pin(0),
        pio_encode_jmp_not_osre(7),
        pio_encode_wait_pin(1, 4),
        pio_encode_wait_irq(0, true, 3),
        pio_encode_in(pio_pins, 8),
        pio_encode_in(pio_isr, 32),
        pio_encode_out(pio_pindirs, 2),
        pio_encode_out(pio_exec_out, 16),
        pio_encode_push(false, true),
        pio_encode_pull(false, false),
        pio_encode_mov_not(pio_x, pio_y),
        pio_encode_mov_reverse(pio_pins, pio_isr),
        pio_encode_irq_clear(false, 5),
        pio_encode_irq_wait(true, 2),
    };
    for (unsigned i = 0; i < sizeof(enc) / sizeof(enc[0]); i++) {
        char src[64];
        snprintf(src, sizeof(src), ".program q\n%s\n", enc[i].src);
        CHECK(pio_assemble(src, "q", &a, err, sizeof(err)) == 0, "%s: %s", enc[i].src, err);
        CHECK(a.instructions[0] == enc[i].word, "%s: assembled %04x, want %04x", enc[i].src,
              (unsigned)a.instructions[0], enc[i].word);
        CHECK(want[i] == enc[i].word, "%s: pio_encode gave %04x, want %04x", enc[i].src, want[i], enc[i].word);
    }
}

static void test_clock_divider(void)
{
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_clkdiv(&c, 2.5f);
    CHECK(c.clkdiv == (2u << 16 | 128u << 8), "%08x", (unsigned)c.clkdiv);
    sm_config_set_clkdiv_int_frac8(&c, 65535, 255);
    CHECK(c.clkdiv == 0xffffff00, "%08x", (unsigned)c.clkdiv);
}

int main(void)
{
    test_pins_low_base();
    test_pins_high_base();
    test_direct_setters_follow_base();
    test_base_conflicts();
    test_pin_masks();
    test_pin_claims();
    test_dma_copy_gives_channel_back();
    test_program_loading();
    test_clock_divider();
    printf("test-pio: %d of %d checks passed\n", checks - fails, checks);
    return fails != 0;
}
