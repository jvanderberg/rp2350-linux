# claude-guest-mcp: an MCP (Model Context Protocol) tool server over stdio, for MicroPython.
#
# Runs on the device that owns the files. Claude Code, running elsewhere, talks to it
# through an ssh session and gets tools to read, write, edit, list and search files and
# to run shell commands here. Newline-delimited JSON-RPC 2.0 on stdin/stdout.
#
#   micropython claude-guest-mcp.py [ROOT]       (default ROOT: current directory)
#
# Works under MicroPython (unix port) and CPython 3.

import sys
import os
import json

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
if not ROOT.startswith("/"):
    ROOT = os.getcwd() + "/" + ROOT
ROOT = ROOT.rstrip("/") or "/"

MAX_READ = 64 * 1024       # bytes returned by read_file in one call
MAX_OUT = 32 * 1024        # bytes of command output kept
DEFAULT_TIMEOUT = 120

TOOLS = [
    {"name": "read_file",
     "description": "Read a text file on the device. Returns lines prefixed with their line number. Use offset/limit for large files.",
     "inputSchema": {"type": "object", "properties": {
         "path": {"type": "string", "description": "File path, absolute or relative to the project root"},
         "offset": {"type": "integer", "description": "First line to return (1-based)"},
         "limit": {"type": "integer", "description": "Number of lines to return"}},
         "required": ["path"]}},
    {"name": "write_file",
     "description": "Create or overwrite a file on the device with the given content. Parent directories are created.",
     "inputSchema": {"type": "object", "properties": {
         "path": {"type": "string"}, "content": {"type": "string"}},
         "required": ["path", "content"]}},
    {"name": "edit_file",
     "description": "Replace an exact string in a file. old_string must occur exactly once unless replace_all is true.",
     "inputSchema": {"type": "object", "properties": {
         "path": {"type": "string"}, "old_string": {"type": "string"}, "new_string": {"type": "string"},
         "replace_all": {"type": "boolean"}},
         "required": ["path", "old_string", "new_string"]}},
    {"name": "list_dir",
     "description": "List a directory on the device. Entries end with / for directories and show file sizes.",
     "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}}},
    {"name": "grep",
     "description": "Search file contents on the device with grep -rn (basic regex). Returns path:line:text.",
     "inputSchema": {"type": "object", "properties": {
         "pattern": {"type": "string"}, "path": {"type": "string", "description": "File or directory, default project root"},
         "ignore_case": {"type": "boolean"}},
         "required": ["pattern"]}},
    {"name": "find",
     "description": "Find files by name pattern (shell glob, e.g. *.c) under a directory on the device.",
     "inputSchema": {"type": "object", "properties": {
         "name": {"type": "string"}, "path": {"type": "string"}},
         "required": ["name"]}},
    {"name": "bash",
     "description": "Run a shell command on the device (BusyBox sh) in the project root. Returns combined stdout/stderr and the exit code.",
     "inputSchema": {"type": "object", "properties": {
         "command": {"type": "string"}, "timeout": {"type": "integer", "description": "Seconds, default 120"}},
         "required": ["command"]}},
    {"name": "delete",
     "description": "Delete a file or an empty directory on the device.",
     "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}},
]


def resolve(p):
    if not p:
        return ROOT
    if not p.startswith("/"):
        p = ROOT + "/" + p
    parts = []
    for seg in p.split("/"):
        if seg == "" or seg == ".":
            continue
        if seg == "..":
            if parts:
                parts.pop()
            continue
        parts.append(seg)
    return "/" + "/".join(parts)


def shq(s):
    return "'" + s.replace("'", "'\\''") + "'"


def isdir(p):
    try:
        return (os.stat(p)[0] & 0xF000) == 0x4000
    except OSError:
        return False


def exists(p):
    try:
        os.stat(p)
        return True
    except OSError:
        return False


def mkdirs(p):
    parts = p.split("/")
    cur = ""
    for seg in parts[:-1]:
        if not seg:
            continue
        cur += "/" + seg
        if not exists(cur):
            os.mkdir(cur)


def read_text(p):
    with open(p, "rb") as f:
        data = f.read()
    try:
        return data.decode("utf-8")
    except Exception:
        raise ValueError("not a UTF-8 text file")


def write_text(p, s):
    mkdirs(p)
    with open(p, "wb") as f:
        f.write(s.encode("utf-8"))


def run_shell(cmd, timeout):
    tmp = "/tmp/.mcp-%d" % os.getpid() if hasattr(os, "getpid") else "/tmp/.mcp-run"
    script = tmp + ".sh"
    out = tmp + ".out"
    write_text(script, cmd + "\n")
    # BusyBox may lack `timeout`, so the limit is a background sleep+kill. 143 = killed.
    # The outer 2>/dev/null keeps the shell's own "Terminated" notices off our stderr.
    full = ("{ cd %s && sh %s > %s 2>&1 & p=$!; { sleep %d; kill $p 2>/dev/null; } & w=$!; "
            "wait $p; rc=$?; kill $w 2>/dev/null; exit $rc; } 2>/dev/null"
            % (shq(ROOT), shq(script), shq(out), int(timeout)))
    st = os.system(full)
    rc = st >> 8 if st > 255 else st
    try:
        with open(out, "rb") as f:
            data = f.read()
    except OSError:
        data = b""
    for p in (script, out):
        try:
            os.remove(p)
        except OSError:
            pass
    text = data.decode("utf-8", "replace") if len(data) <= MAX_OUT else \
        data[:MAX_OUT].decode("utf-8", "replace") + "\n...[output truncated at %d bytes]" % MAX_OUT
    return text, rc


# ---- tools -------------------------------------------------------------

def t_read_file(a):
    p = resolve(a.get("path"))
    text = read_text(p)
    lines = text.split("\n")
    if text.endswith("\n"):
        lines.pop()
    off = int(a.get("offset") or 1)
    lim = int(a.get("limit") or 0)
    if off < 1:
        off = 1
    end = len(lines) if lim <= 0 else min(len(lines), off - 1 + lim)
    out = []
    size = 0
    for i in range(off - 1, end):
        ln = "%d\t%s" % (i + 1, lines[i])
        size += len(ln) + 1
        if size > MAX_READ:
            out.append("...[truncated: %d more lines; use offset=%d]" % (len(lines) - i, i + 1))
            break
        out.append(ln)
    if not out and len(lines) == 0:
        return "(empty file)"
    return "\n".join(out)


def t_write_file(a):
    p = resolve(a.get("path"))
    content = a.get("content", "")
    write_text(p, content)
    return "wrote %d bytes to %s" % (len(content.encode("utf-8")), p)


def t_edit_file(a):
    p = resolve(a.get("path"))
    old = a.get("old_string", "")
    new = a.get("new_string", "")
    if old == "":
        raise ValueError("old_string must not be empty")
    text = read_text(p)
    n = text.count(old)
    if n == 0:
        raise ValueError("old_string not found in %s" % p)
    if n > 1 and not a.get("replace_all"):
        raise ValueError("old_string occurs %d times in %s; make it unique or set replace_all" % (n, p))
    text = text.replace(old, new) if a.get("replace_all") else text.replace(old, new, 1)
    write_text(p, text)
    return "replaced %d occurrence%s in %s" % (n, "" if n == 1 else "s", p)


def t_list_dir(a):
    p = resolve(a.get("path"))
    if not isdir(p):
        raise ValueError("not a directory: %s" % p)
    names = sorted(os.listdir(p))
    out = []
    for n in names:
        fp = p + "/" + n
        try:
            st = os.stat(fp)
        except OSError:
            out.append(n + "  ?")
            continue
        if (st[0] & 0xF000) == 0x4000:
            out.append(n + "/")
        else:
            out.append("%s  %d" % (n, st[6]))
    return p + "\n" + ("\n".join(out) if out else "(empty)")


def t_grep(a):
    p = resolve(a.get("path"))
    flags = "-rn" + ("i" if a.get("ignore_case") else "")
    # BusyBox grep has no --exclude-dir; drop .git hits afterwards.
    cmd = "grep %s -e %s %s 2>/dev/null | grep -v '/\\.git/' | head -n 500" % (flags, shq(a["pattern"]), shq(p))
    text, rc = run_shell(cmd, 60)
    return text if text.strip() else "(no matches)"


def t_find(a):
    p = resolve(a.get("path"))
    cmd = "find %s -name .git -prune -o -name %s -print 2>/dev/null | head -n 500" % (shq(p), shq(a["name"]))
    text, rc = run_shell(cmd, 60)
    return text if text.strip() else "(no matches)"


def t_bash(a):
    text, rc = run_shell(a["command"], a.get("timeout") or DEFAULT_TIMEOUT)
    if rc in (124, 143):
        text += "\n[timed out]"
    return text + ("\n[exit %d]" % rc if rc else "")


def t_delete(a):
    p = resolve(a.get("path"))
    if p == ROOT:
        raise ValueError("refusing to delete the project root")
    if isdir(p):
        os.rmdir(p)
    else:
        os.remove(p)
    return "deleted " + p


HANDLERS = {
    "read_file": t_read_file, "write_file": t_write_file, "edit_file": t_edit_file,
    "list_dir": t_list_dir, "grep": t_grep, "find": t_find, "bash": t_bash, "delete": t_delete,
}

# ---- JSON-RPC loop -----------------------------------------------------

def send(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    try:
        sys.stdout.flush()
    except Exception:
        pass


def reply(req_id, result):
    send({"jsonrpc": "2.0", "id": req_id, "result": result})


def error(req_id, code, msg):
    send({"jsonrpc": "2.0", "id": req_id, "error": {"code": code, "message": msg}})


def handle(req):
    method = req.get("method")
    req_id = req.get("id")
    params = req.get("params") or {}
    if method == "initialize":
        reply(req_id, {
            "protocolVersion": params.get("protocolVersion", "2024-11-05"),
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "claude-guest-mcp", "version": "0.1"},
            "instructions": "The project files live on this device at %s. Use these tools for all file "
                            "and shell operations; the files are not reachable any other way." % ROOT,
        })
    elif method == "ping":
        reply(req_id, {})
    elif method == "tools/list":
        reply(req_id, {"tools": TOOLS})
    elif method == "tools/call":
        name = params.get("name")
        args = params.get("arguments") or {}
        fn = HANDLERS.get(name)
        if fn is None:
            error(req_id, -32601, "unknown tool: %s" % name)
            return
        try:
            text = fn(args)
            reply(req_id, {"content": [{"type": "text", "text": text}]})
        except Exception as e:
            reply(req_id, {"content": [{"type": "text", "text": "error: %s" % e}], "isError": True})
    elif req_id is None:
        pass  # notification (notifications/initialized, notifications/cancelled, ...)
    else:
        error(req_id, -32601, "method not found: %s" % method)


def main():
    while True:
        line = sys.stdin.readline()
        if not line:
            break
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception:
            error(None, -32700, "parse error")
            continue
        handle(req)


main()
