#!/usr/bin/env python3
"""Run one shell command on the board's Linux console and print its output.

usage: board-cmd.py [--port PORT] [--timeout S] [--no-login] [--force] COMMAND...

Talks to rvemu-pico-shell's USB console (default: the first
/dev/cu.usbmodemrvemu*). It drops whatever the firmware had buffered, wakes
the shell (logging in as root if it sits at `login:`), then sends one line:

    echo RVB""EGIN_<tag>; COMMAND; echo RVE""ND_<tag> $?

The terminal echoes that line back with the quotes in it, so only the
commands' own output can contain RVBEGIN_<tag> / RVEND_<tag>. What lies
between the two markers is printed; the exit status is COMMAND's.

The port takes one program at a time: close screen first (Ctrl-A k).
Never opens it at 1200 baud (that would reboot the board into BOOTSEL).
No pyserial: plain termios, raw mode.
"""
import argparse
import glob
import os
import re
import secrets
import select
import subprocess
import sys
import termios
import time
import tty

# The firmware's input ring holds 256 bytes; feed it slower than the guest
# drains its UART.
CHUNK = 32
CHUNK_GAP_S = 0.01
PROMPT = re.compile(rb"(?:[#$] |login: )$")


def default_port():
    ports = sorted(glob.glob("/dev/cu.usbmodemrvemu*"))
    return ports[0] if ports else None


def holders(port):
    """PIDs of other programs that have `port` open (lsof)."""
    r = subprocess.run(["lsof", "-t", port], capture_output=True, text=True, check=False)
    return [p for p in r.stdout.split() if p != str(os.getpid())]


class Console:
    def __init__(self, port):
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd, termios.TCSANOW)
        attrs = termios.tcgetattr(self.fd)
        attrs[4] = attrs[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self.buf = b""

    def read(self, timeout):
        """Append what arrives within `timeout` s; False if nothing did."""
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return False
        data = os.read(self.fd, 4096)
        if not data:
            raise ConnectionError("console port closed")
        self.buf += data
        return True

    def drain(self, quiet=0.3, limit=5.0):
        """Read until the port has been quiet for `quiet` s."""
        end = time.time() + limit
        while time.time() < end and self.read(quiet):
            pass

    def send(self, s):
        b = s.encode()
        for i in range(0, len(b), CHUNK):
            os.write(self.fd, b[i : i + CHUNK])
            time.sleep(CHUNK_GAP_S)

    def expect(self, pat, timeout):
        """Wait until `pat` matches the buffer; the match, or None on timeout."""
        end = time.time() + timeout
        while True:
            m = pat.search(self.buf)
            if m:
                return m
            left = end - time.time()
            if left <= 0:
                return None
            self.read(min(left, 0.5))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", default=None)
    ap.add_argument("--timeout", type=float, default=60.0, help="seconds for COMMAND to finish")
    ap.add_argument("--no-login", action="store_true", help="fail at login: instead of logging in as root")
    ap.add_argument("--force", action="store_true", help="skip the check that nothing else has the port open")
    ap.add_argument("command", nargs="+")
    a = ap.parse_args()

    port = a.port or default_port()
    if not port:
        sys.exit("board-cmd: no /dev/cu.usbmodemrvemu* (is rvemu-pico-shell running? scripts/pico-shell.sh)")
    if not a.force and holders(port):
        sys.exit(f"board-cmd: {port} is open in another program (pid {' '.join(holders(port))}); "
                 "close it (screen: Ctrl-A k)")
    con = Console(port)

    # Output buffered while no terminal was open arrives first: drop it.
    con.drain()
    con.buf = b""
    con.send("\r")
    if not con.expect(PROMPT, 10):
        sys.exit(f"board-cmd: no shell prompt; last output: {con.buf[-200:]!r}")
    if con.buf.endswith(b"login: "):
        if a.no_login:
            sys.exit("board-cmd: the console is at login:")
        con.buf = b""
        con.send("root\r")
        if not con.expect(re.compile(rb"[#$] $"), 30):
            sys.exit(f"board-cmd: login failed; last output: {con.buf[-200:]!r}")

    tag = secrets.token_hex(4)
    cmd = " ".join(a.command)
    con.buf = b""
    con.send(f'echo RVB""EGIN_{tag}; {cmd}; echo RVE""ND_{tag} $?\r')
    begin = con.expect(re.compile(rb"RVBEGIN_" + tag.encode() + rb"\r?\n"), 10)
    if not begin:
        sys.exit(f"board-cmd: the command did not start; last output: {con.buf[-200:]!r}")
    con.buf = con.buf[begin.end() :]
    end = con.expect(re.compile(rb"RVEND_" + tag.encode() + rb" (\d+)\r?\n"), a.timeout)
    out = con.buf[: end.start()] if end else con.buf
    sys.stdout.write(out.replace(b"\r\n", b"\n").decode(errors="replace"))
    sys.stdout.flush()
    if not end:
        # Leave the shell usable: interrupt whatever is still running.
        con.send("\x03")
        sys.exit(f"board-cmd: timeout after {a.timeout:.0f} s (sent Ctrl-C)")
    os.close(con.fd)
    return int(end.group(1))


if __name__ == "__main__":
    sys.exit(main())
