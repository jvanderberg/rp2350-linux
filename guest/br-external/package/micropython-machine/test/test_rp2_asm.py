"""rp2.py's assembler (the rp2 port's) against libpio's, which is checked
against pio-parser: the same instructions in each one's syntax must give the
same words. Runs in CPython on the host, with MicroPython's modules stubbed;
rp2's register side is tested on a real block by guest/tests/pio.

    python3 -m unittest discover -s guest/br-external/package/micropython-machine/test
"""
import os
import subprocess
import sys
import types
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
LIBPIO_TEST = os.path.join(HERE, "../../libpio/test")

# MicroPython's modules, as far as importing rp2.py needs them
for name, attrs in {
    "micropython": {"const": lambda x: x},
    "_rvmem": {},
    "uctypes": {},
}.items():
    sys.modules.setdefault(name, types.ModuleType(name)).__dict__.update(attrs)
sys.path.insert(0, os.path.dirname(HERE))
import rp2  # noqa: E402

# (rp2 syntax, pioasm syntax): one instruction each
SINGLE = [
    ("nop()", "nop"),
    ("set(pins, 1)", "set pins, 1"),
    ("set(x, 31)", "set x, 31"),
    ("set(y, 0)", "set y, 0"),
    ("set(pindirs, 3)", "set pindirs, 3"),
    ("mov(x, y)", "mov x, y"),
    ("mov(y, invert(x))", "mov y, ~x"),
    ("mov(pins, reverse(isr))", "mov pins, ::isr"),
    ("mov(isr, null)", "mov isr, null"),
    ("mov(osr, status)", "mov osr, status"),
    ("mov(exec, x)", "mov exec, x"),
    ("mov(pc, y)", "mov pc, y"),
    # the documentation pass's finding: this was assembled as mov exec, x
    ("mov(pindirs, x)", "mov pindirs, x"),
    ("mov(pindirs, invert(null))", "mov pindirs, ~null"),
    ("in_(pins, 8)", "in pins, 8"),
    ("in_(x, 32)", "in x, 32"),
    ("in_(null, 1)", "in null, 1"),
    ("in_(isr, 4)", "in isr, 4"),
    ("in_(osr, 12)", "in osr, 12"),
    ("out(pins, 1)", "out pins, 1"),
    ("out(pindirs, 2)", "out pindirs, 2"),
    ("out(x, 32)", "out x, 32"),
    ("out(null, 7)", "out null, 7"),
    ("out(pc, 5)", "out pc, 5"),
    ("out(isr, 3)", "out isr, 3"),
    ("out(exec, 16)", "out exec, 16"),
    ("push()", "push"),
    ("push(noblock)", "push noblock"),
    ("push(iffull)", "push iffull"),
    ("push(iffull, noblock)", "push iffull noblock"),
    ("pull()", "pull"),
    ("pull(noblock)", "pull noblock"),
    ("pull(ifempty, block)", "pull ifempty block"),
    ("wait(1, gpio, 5)", "wait 1 gpio, 5"),
    ("wait(0, pin, 3)", "wait 0 pin, 3"),
    ("wait(1, irq, 2)", "wait 1 irq, 2"),
    ("wait(0, irq, rel(3))", "wait 0 irq, 3 rel"),
    ("irq(4)", "irq 4"),
    ("irq(block, 2)", "irq wait 2"),
    ("irq(clear, 5)", "irq clear 5"),
    ("irq(rel(1))", "irq 1 rel"),
    ("irq(clear, rel(6))", "irq clear 6 rel"),
    ("nop() [7]", "nop [7]"),
    ("set(x, 1) [31]", "set x, 1 [31]"),
]

# whole programs (labels, jmps, wrap, side-set): rp2 body, pioasm body
PROGRAMS = [
    (
        "sideset_init=rp2.PIO.OUT_LOW",
        """
    wrap_target()
    label("bitloop")
    out(x, 1)               .side(0) [2]
    jmp(not_x, "do_zero")   .side(1) [1]
    jmp("bitloop")          .side(1) [4]
    label("do_zero")
    nop()                   .side(0) [4]
    wrap()
""",
        """.side_set 1
.wrap_target
bitloop:
    out x, 1       side 0 [2]
    jmp !x do_zero side 1 [1]
    jmp bitloop    side 1 [4]
do_zero:
    nop            side 0 [4]
.wrap
""",
    ),
    (
        "",
        """
    label("top")
    set(x, 7)
    label("loop")
    jmp(x_dec, "loop")
    jmp(not_y, "top")
    jmp(y_dec, "top")
    jmp(x_not_y, "loop")
    jmp(pin, "top")
    jmp(not_osre, "top")
    wrap_target()
    mov(y, x)
    wrap()
""",
        """top:
    set x, 7
loop:
    jmp x-- loop
    jmp !y top
    jmp y-- top
    jmp x!=y loop
    jmp pin top
    jmp !osre top
.wrap_target
    mov y, x
.wrap
""",
    ),
]


def libpio(src):
    """{program name: (wrap target, wrap, [words])} from libpio's assembler."""
    subprocess.run(["make", "-s", "-C", LIBPIO_TEST, "build/asm-dump"], check=True)
    r = subprocess.run([os.path.join(LIBPIO_TEST, "build/asm-dump")], input=src, text=True,
                       capture_output=True)
    if r.returncode:
        raise AssertionError("libpio: " + r.stderr)
    out = {}
    for line in r.stdout.splitlines():
        head, words = line.split(" :")
        f = head.split()
        out[f[0]] = (int(f[2]), int(f[3]), [int(w, 16) for w in words.split()])
    return out


def rp2_program(kw, body):
    ns = {"rp2": rp2}
    exec("@rp2.asm_pio(%s)\ndef p():\n%s" % (kw, body), ns)
    prog = ns["p"]
    execctrl = prog[4]
    return (execctrl >> 7 & 31, execctrl >> 12 & 31, list(prog[0]))


class Rp2Assembler(unittest.TestCase):
    def test_single_instructions(self):
        src = "".join(".program i%d\n%s\n" % (i, p) for i, (_, p) in enumerate(SINGLE))
        want = libpio(src)
        for i, (r, p) in enumerate(SINGLE):
            with self.subTest(r):
                got = rp2.asm_pio_encode(r, 0)
                self.assertEqual(got, want["i%d" % i][2][0], "%s: rp2 %04x, libpio (%s) %04x"
                                 % (r, got, p, want["i%d" % i][2][0]))

    def test_programs(self):
        for k, (kw, body, pio) in enumerate(PROGRAMS):
            with self.subTest(k):
                target, wrap, words = libpio(".program p\n" + pio)["p"]
                self.assertEqual(rp2_program(kw, body), (target, wrap, words))


if __name__ == "__main__":
    unittest.main()
