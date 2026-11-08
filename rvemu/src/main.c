/* rvemu: RV32IMA + Sv32 Linux system emulator, prototype for an RP2350
 * (Hazard3) port. Boots a kernel Image with a generated device tree and an
 * optional initramfs, and reports instruction counts plus a timing model. */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cpu.h"
#include "dev.h"

#define DTB_RESERVE 0x10000u /* top 64 KiB of RAM holds the DTB */

static void die(const char *msg)
{
    dev_shutdown();
    fprintf(stderr, "rvemu: %s\n", msg);
    exit(1);
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "rvemu: cannot open %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n > 0 ? (size_t) n : 1);
    if (!buf || fread(buf, 1, (size_t) n, f) != (size_t) n) {
        fprintf(stderr, "rvemu: cannot read %s\n", path);
        exit(1);
    }
    fclose(f);
    *size = (size_t) n;
    return buf;
}

static char *make_dts(uint32_t ram_base, uint32_t ram_size,
                      const char *bootargs, uint32_t initrd_start,
                      uint32_t initrd_end)
{
    char initrd[160] = "";
    if (initrd_end > initrd_start)
        snprintf(initrd, sizeof(initrd),
                 "    linux,initrd-start = <0x%x>;\n"
                 "    linux,initrd-end = <0x%x>;\n",
                 initrd_start, initrd_end);
    char *s = NULL;
    int n = asprintf(
        &s,
        "/dts-v1/;\n"
        "/ {\n"
        "  #address-cells = <1>;\n"
        "  #size-cells = <1>;\n"
        "  model = \"rvemu\";\n"
        "  compatible = \"rvemu\";\n"
        "  chosen {\n"
        "    bootargs = \"%s\";\n"
        "    stdout-path = \"/soc/serial@f4000000\";\n"
        "%s"
        "  };\n"
        "  cpus {\n"
        "    #address-cells = <1>;\n"
        "    #size-cells = <0>;\n"
        "    timebase-frequency = <%u>;\n"
        "    cpu0: cpu@0 {\n"
        "      device_type = \"cpu\";\n"
        "      compatible = \"riscv\";\n"
        "      reg = <0>;\n"
        "      status = \"okay\";\n"
        "      riscv,isa = \"rv32ima\";\n"
        "      mmu-type = \"riscv,sv32\";\n"
        "      cpu0_intc: interrupt-controller {\n"
        "        #interrupt-cells = <1>;\n"
        "        #address-cells = <0>;\n"
        "        interrupt-controller;\n"
        "        compatible = \"riscv,cpu-intc\";\n"
        "      };\n"
        "    };\n"
        "  };\n"
        "  memory@%x {\n"
        "    device_type = \"memory\";\n"
        "    reg = <0x%x 0x%x>;\n"
        "  };\n"
        "  soc {\n"
        "    #address-cells = <1>;\n"
        "    #size-cells = <1>;\n"
        "    compatible = \"simple-bus\";\n"
        "    ranges;\n"
        "    plic0: interrupt-controller@f0000000 {\n"
        "      compatible = \"sifive,plic-1.0.0\";\n"
        "      reg = <0x%x 0x%x>;\n"
        "      #interrupt-cells = <1>;\n"
        "      #address-cells = <0>;\n"
        "      interrupt-controller;\n"
        "      interrupts-extended = <&cpu0_intc 9>;\n"
        "      riscv,ndev = <31>;\n"
        "    };\n"
        "    serial@f4000000 {\n"
        "      compatible = \"ns16550\";\n"
        "      reg = <0x%x 0x%x>;\n"
        "      interrupts = <%d>;\n"
        "      interrupt-parent = <&plic0>;\n"
        "      clock-frequency = <5000000>;\n"
        "      no-loopback-test;\n"
        "    };\n"
        "  };\n"
        "};\n",
        bootargs, initrd, TIMEBASE_HZ, ram_base, ram_base, ram_size,
        PLIC_BASE, PLIC_SIZE, UART_BASE, UART_SIZE, UART_IRQ);
    if (n < 0)
        die("out of memory");
    return s;
}

/* Compile DTS text with the host dtc. */
static uint8_t *compile_dts(const char *dts, size_t *size)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp)
        tmp = "/tmp";
    char src[1024], out[1024], cmd[2304];
    snprintf(src, sizeof(src), "%s/rvemu-XXXXXX", tmp);
    int fd = mkstemp(src);
    if (fd < 0)
        die("mkstemp failed");
    FILE *f = fdopen(fd, "w");
    fputs(dts, f);
    fclose(f);
    snprintf(out, sizeof(out), "%s.dtb", src);
    snprintf(cmd, sizeof(cmd), "dtc -q -I dts -O dtb -o '%s' '%s'", out, src);
    int rc = system(cmd);
    unlink(src);
    if (rc != 0)
        die("dtc failed (is dtc installed?)");
    uint8_t *blob = read_file(out, size);
    unlink(out);
    return blob;
}

/* ---------------------------------------------------------------- report */

/* Timing model for RP2350 Hazard3 at 300 MHz. These constants are estimates,
 * not measurements; see README. Instruction mix and XIP cache misses come
 * from this run. */
#define HOST_HZ 300e6
#define PSRAM_MISS_CYCLES 100.0
#define INTERP_CYCLES_PER_INSN 55.0
#define JIT_ALU 1.4
#define JIT_MEM 14.0
#define JIT_BRANCH 4.0
#define JIT_JAL 4.0
#define JIT_JALR 20.0
#define JIT_AMO 40.0
#define JIT_SYSTEM 150.0
#define JIT_TRAP 200.0
#define JIT_TRANSLATE_PER_INSN 150.0
#define JIT_RETRANSLATE_FACTOR 2.0

static double secs(const struct timespec *a, const struct timespec *b)
{
    return (double) (b->tv_sec - a->tv_sec) + (double) (b->tv_nsec - a->tv_nsec) * 1e-9;
}

static void report(const cpu_t *c, double host_s)
{
    const stats_t *s = &c->stats;
    const uint64_t *op = s->op;
    uint64_t n = c->instret;
    uint64_t alu = op[0x13] + op[0x33] + op[0x37] + op[0x17];
    uint64_t ld = op[0x03], st = op[0x23], br = op[0x63], jal = op[0x6F],
             jalr = op[0x67], amo = op[0x2F], sys = op[0x73] + op[0x0F];
    double pct = n ? 100.0 / (double) n : 0;

    fprintf(stderr, "\n==== rvemu stats ====\n");
    fprintf(stderr, "host time          %.2f s  (%.1f MIPS)\n", host_s,
            host_s > 0 ? (double) n / host_s / 1e6 : 0);
    fprintf(stderr, "guest instructions %llu\n", (unsigned long long) n);
    if (s->user_instret)
        fprintf(stderr, "  to first U-mode  %llu  (unique insns %llu)\n",
                (unsigned long long) s->user_instret,
                (unsigned long long) s->user_unique_insns);
    if (s->until_instret)
        fprintf(stderr, "  to --until match %llu  (unique insns %llu)\n",
                (unsigned long long) s->until_instret,
                (unsigned long long) s->until_unique_insns);
    fprintf(stderr, "unique insns run   %llu (%.0f KiB of code, %llu pages)\n",
            (unsigned long long) s->unique_insns, (double) s->unique_insns * 4 / 1024,
            (unsigned long long) s->unique_code_pages);
    fprintf(stderr,
            "mix  alu %.1f%%  load %.1f%%  store %.1f%%  branch %.1f%%  jal %.1f%%"
            "  jalr %.1f%%  amo %.2f%%  system %.2f%%\n",
            alu * pct, ld * pct, st * pct, br * pct, jal * pct, jalr * pct,
            amo * pct, sys * pct);
    fprintf(stderr, "tlb misses  R %llu  W %llu  X %llu   flushes %llu  page flushes %llu\n",
            (unsigned long long) s->tlb_miss[ACC_R],
            (unsigned long long) s->tlb_miss[ACC_W],
            (unsigned long long) s->tlb_miss[ACC_X],
            (unsigned long long) s->tlb_flushes,
            (unsigned long long) s->tlb_page_flushes);
    fprintf(stderr,
            "traps  exceptions %llu  interrupts %llu  sbi %llu  wfi %llu"
            "  idle ticks %llu (%.2f s guest)\n",
            (unsigned long long) s->exceptions, (unsigned long long) s->interrupts,
            (unsigned long long) s->sbi_calls, (unsigned long long) s->wfi,
            (unsigned long long) s->idle_ticks,
            (double) s->idle_ticks / TIMEBASE_HZ);

    if (!c->model)
        return;
    const xip_cache_t *xi = &c->xip_interp, *xj = &c->xip_jit;
    double interp = (double) n * INTERP_CYCLES_PER_INSN +
                    (double) xi->misses * PSRAM_MISS_CYCLES;
    double jit = (double) alu * JIT_ALU + (double) (ld + st) * JIT_MEM +
                 (double) br * JIT_BRANCH + (double) jal * JIT_JAL +
                 (double) jalr * JIT_JALR + (double) amo * JIT_AMO +
                 (double) sys * JIT_SYSTEM +
                 (double) (s->exceptions + s->interrupts) * JIT_TRAP +
                 (double) s->unique_insns * JIT_TRANSLATE_PER_INSN *
                     JIT_RETRANSLATE_FACTOR +
                 (double) xj->misses * PSRAM_MISS_CYCLES;
    fprintf(stderr, "\n---- RP2350 @300 MHz timing model (estimate) ----\n");
    fprintf(stderr, "XIP cache, interpreter (fetch+data): %llu misses / %llu (%.2f%%)\n",
            (unsigned long long) xi->misses, (unsigned long long) xi->accesses,
            xi->accesses ? 100.0 * (double) xi->misses / (double) xi->accesses : 0);
    fprintf(stderr, "XIP cache, translator (data only):   %llu misses / %llu (%.2f%%)\n",
            (unsigned long long) xj->misses, (unsigned long long) xj->accesses,
            xj->accesses ? 100.0 * (double) xj->misses / (double) xj->accesses : 0);
    fprintf(stderr, "interpreter  %.1f cycles/insn  -> %.1f s\n",
            n ? interp / (double) n : 0, interp / HOST_HZ);
    fprintf(stderr, "translator   %.1f cycles/insn  -> %.1f s\n",
            n ? jit / (double) n : 0, jit / HOST_HZ);
    fprintf(stderr, "(guest time excludes %.2f s of idle WFI)\n",
            (double) s->idle_ticks / TIMEBASE_HZ);
}

/* ---------------------------------------------------------------- main */

static void usage(void)
{
    fprintf(stderr,
            "usage: rvemu -k Image [-i initrd] [options]\n"
            "  -k FILE        kernel Image (loaded at start of RAM)\n"
            "  -i FILE        initramfs (cpio)\n"
            "  -b FILE        use this DTB instead of generating one\n"
            "  -m MB          guest RAM size (default 64)\n"
            "  -a ARGS        kernel command line (default \"earlycon console=ttyS0\")\n"
            "  -u STRING      stop when the console prints STRING (e.g. \"login:\")\n"
            "  -n COUNT       stop after COUNT guest instructions\n"
            "  -M             enable the RP2350 timing model (XIP cache sim)\n"
            "  -B             batch mode: no keyboard input, idle time skipped\n"
            "  -D             print the generated device tree and exit\n"
            "Ctrl-A x exits.\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *kernel = NULL, *initrd_path = NULL, *dtb_path = NULL;
    const char *bootargs = "earlycon console=ttyS0", *until = NULL;
    uint32_t ram_mb = 64;
    uint64_t max_insns = 0;
    bool model = false, batch = false, dump_dts = false;

    int opt;
    while ((opt = getopt(argc, argv, "k:i:b:m:a:u:n:MBDh")) != -1) {
        switch (opt) {
        case 'k': kernel = optarg; break;
        case 'i': initrd_path = optarg; break;
        case 'b': dtb_path = optarg; break;
        case 'm': ram_mb = (uint32_t) strtoul(optarg, NULL, 0); break;
        case 'a': bootargs = optarg; break;
        case 'u': until = optarg; break;
        case 'n': max_insns = strtoull(optarg, NULL, 0); break;
        case 'M': model = true; break;
        case 'B': batch = true; break;
        case 'D': dump_dts = true; break;
        default: usage();
        }
    }
    if (!kernel && !dump_dts)
        usage();
    if (ram_mb < 4 || ram_mb > 1024)
        die("RAM size must be 4..1024 MB");

    const uint32_t ram_base = 0;
    const uint32_t ram_size = ram_mb << 20;
    const uint32_t dtb_addr = ram_size - DTB_RESERVE;

    size_t initrd_size = 0;
    uint8_t *initrd = initrd_path ? read_file(initrd_path, &initrd_size) : NULL;
    uint32_t initrd_addr = 0;
    if (initrd) {
        if (initrd_size + DTB_RESERVE > ram_size / 2)
            die("initrd too large for RAM size");
        initrd_addr = (uint32_t) ((dtb_addr - initrd_size) & ~0xFFFu);
    }

    char *dts = make_dts(ram_base, ram_size, bootargs, ram_base + initrd_addr,
                         initrd ? ram_base + initrd_addr + (uint32_t) initrd_size : 0);
    if (dump_dts) {
        fputs(dts, stdout);
        return 0;
    }

    uint8_t *ram = calloc(ram_size, 1);
    if (!ram)
        die("cannot allocate guest RAM");

    size_t ksize;
    uint8_t *kimg = read_file(kernel, &ksize);
    if (ksize > (initrd ? initrd_addr : dtb_addr))
        die("kernel does not fit in RAM");
    memcpy(ram, kimg, ksize);
    free(kimg);
    if (initrd) {
        memcpy(ram + initrd_addr, initrd, initrd_size);
        free(initrd);
    }

    size_t dtb_size;
    uint8_t *dtb = dtb_path ? read_file(dtb_path, &dtb_size) : compile_dts(dts, &dtb_size);
    if (dtb_size > DTB_RESERVE)
        die("DTB too large");
    memcpy(ram + dtb_addr, dtb, dtb_size);
    free(dtb);
    free(dts);

    static cpu_t cpu;
    cpu_t *c = &cpu;
    cpu_init(c, ram, ram_base, ram_size, ram_base, ram_base + dtb_addr);
    if (model) {
        /* RP2350 XIP cache: 16 KiB, 2-way, 8-byte lines (see README) */
        c->model = true;
        xip_init(&c->xip_interp, 16384, 2, 8);
        xip_init(&c->xip_jit, 16384, 2, 8);
    }
    dev_init(c, !batch, until);

    fprintf(stderr, "[rvemu] RAM %u MiB, kernel %zu bytes, initrd %zu bytes @0x%x, dtb @0x%x\n",
            ram_mb, ksize, initrd_size, ram_base + initrd_addr, ram_base + dtb_addr);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!c->halt) {
        for (int i = 0; i < 4096 && !c->halt; i++) {
            cpu_check_irq(c);
            cpu_step(c);
        }
        dev_poll(c);
        if (max_insns && c->instret >= max_insns)
            c->halt = HALT_MAX_INSNS;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    dev_shutdown();

    static const char *why[] = {"", "guest shutdown", "--until matched",
                                "instruction limit", "guest idle", "user exit",
                                "fatal"};
    fprintf(stderr, "\n[rvemu] stopped: %s at pc=0x%08x priv=%c\n", why[c->halt],
            c->pc, c->priv == PRIV_S ? 'S' : 'U');
    report(c, secs(&t0, &t1));
    return 0;
}
