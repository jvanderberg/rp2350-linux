#!/usr/bin/env python3
"""Copy a file to the board's guest over the console.

usage: push.py LOCAL REMOTE

Runs `base64 -d > REMOTE` in the guest's shell with echo off and streams
LOCAL to it as base64 lines, then EOF. While base64 reads the terminal,
the shell's line editor (which takes at most 1 KiB per line) is not in the
way, and nothing comes back but the result. Uses board-cmd.py's console
handling (lock check, login) and needs the board lock the same way.
"""
import base64
import importlib.util
import os
import re
import secrets
import subprocess
import sys
import time

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../../../rp2350/tools")
_spec = importlib.util.spec_from_file_location("board_cmd", os.path.join(TOOLS, "board-cmd.py"))
bc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bc)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().split("\n")[2])
    local, remote = sys.argv[1:]
    lock = os.path.join(TOOLS, "board-lock")
    if subprocess.run([lock, "check", os.environ.get("BOARD_LOCK_OWNER", "nobody")], check=False).returncode:
        sys.exit(1)
    port = bc.default_port()
    if not port:
        sys.exit("push: no /dev/cu.usbmodemrvemu* (is rvemu-pico-shell running?)")
    if bc.holders(port):
        sys.exit(f"push: {port} is open in another program")
    con = bc.Console(port)
    con.drain()
    con.buf = b""
    con.send("\r")
    if not con.expect(bc.PROMPT, 10):
        sys.exit(f"push: no shell prompt; last output: {con.buf[-200:]!r}")
    if con.buf.endswith(b"login: "):
        con.buf = b""
        con.send("root\r")
        if not con.expect(re.compile(rb"[#$] $"), 30):
            sys.exit("push: login failed")
    tag = secrets.token_hex(4)
    con.buf = b""
    con.send(f'stty -echo; echo RVRE""ADY_{tag}; base64 -d > {remote}; '
             f'r=$?; stty echo; echo RVDO""NE_{tag} $r\r')
    if not con.expect(re.compile(rb"RVREADY_" + tag.encode() + rb"\r?\n"), 10):
        sys.exit(f"push: base64 did not start; last output: {con.buf[-200:]!r}")
    data = open(local, "rb").read()
    text = base64.encodebytes(data).decode()  # 76-character lines
    t0 = time.time()
    con.send(text)
    con.send("\x04")
    done = con.expect(re.compile(rb"RVDONE_" + tag.encode() + rb" (\d+)\r?\n"), 60)
    if not done:
        con.send("\x03")
        sys.exit(f"push: no result; last output: {con.buf[-200:]!r}")
    rc = int(done.group(1))
    print(f"push: {len(data)} bytes to {remote} in {time.time() - t0:.1f} s, rc {rc}")
    os.close(con.fd)
    return rc


if __name__ == "__main__":
    sys.exit(main())
