//! The tools Claude gets: files under the project root, and a shell there.

use crate::json::Value;
use crate::sys;
use alloc::format;
use alloc::string::{String, ToString};
use alloc::vec::Vec;

const MAX_READ: usize = 64 * 1024; // bytes returned by read_file in one call
const MAX_OUT: usize = 32 * 1024; // bytes of command output kept
const DEFAULT_TIMEOUT: i64 = 120;

pub const TOOLS_JSON: &str = r#"[{"name":"read_file","description":"Read a text file on the device. Returns lines prefixed with their line number. Use offset/limit for large files.","inputSchema":{"type":"object","properties":{"path":{"type":"string","description":"File path, absolute or relative to the project root"},"offset":{"type":"integer","description":"First line to return (1-based)"},"limit":{"type":"integer","description":"Number of lines to return"}},"required":["path"]}},{"name":"write_file","description":"Create or overwrite a file on the device with the given content. Parent directories are created.","inputSchema":{"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]}},{"name":"edit_file","description":"Replace an exact string in a file. old_string must occur exactly once unless replace_all is true.","inputSchema":{"type":"object","properties":{"path":{"type":"string"},"old_string":{"type":"string"},"new_string":{"type":"string"},"replace_all":{"type":"boolean"}},"required":["path","old_string","new_string"]}},{"name":"list_dir","description":"List a directory on the device. Entries end with / for directories and show file sizes.","inputSchema":{"type":"object","properties":{"path":{"type":"string"}}}},{"name":"grep","description":"Search file contents on the device with grep -rn (basic regex). Returns path:line:text.","inputSchema":{"type":"object","properties":{"pattern":{"type":"string"},"path":{"type":"string","description":"File or directory, default project root"},"ignore_case":{"type":"boolean"}},"required":["pattern"]}},{"name":"find","description":"Find files by name pattern (shell glob, e.g. *.c) under a directory on the device.","inputSchema":{"type":"object","properties":{"name":{"type":"string"},"path":{"type":"string"}},"required":["name"]}},{"name":"bash","description":"Run a shell command on the device (BusyBox sh) in the project root. Returns combined stdout/stderr and the exit code.","inputSchema":{"type":"object","properties":{"command":{"type":"string"},"timeout":{"type":"integer","description":"Seconds, default 120"}},"required":["command"]}},{"name":"delete","description":"Delete a file or an empty directory on the device.","inputSchema":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}}]"#;

pub type Out = Result<String, String>;

pub struct Ctx {
    pub root: String,
    pub envp: *const *const u8,
}

fn oserr(what: &str, path: &str, e: i32) -> String {
    format!("{what} {path}: {}", sys::errno_text(e))
}

/// An absolute, normalized path: relative ones hang off the project root.
pub fn resolve(root: &str, p: Option<&str>) -> String {
    let p = match p {
        None | Some("") => return root.to_string(),
        Some(p) => p,
    };
    let joined = if p.starts_with('/') { p.to_string() } else { format!("{root}/{p}") };
    let mut parts: Vec<&str> = Vec::new();
    for seg in joined.split('/') {
        match seg {
            "" | "." => {}
            ".." => {
                parts.pop();
            }
            s => parts.push(s),
        }
    }
    let mut out = String::new();
    for s in &parts {
        out.push('/');
        out.push_str(s);
    }
    if out.is_empty() { out.push('/') }
    out
}

/// Single quotes for the shell.
fn shq(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('\'');
    for c in s.chars() {
        if c == '\'' { out.push_str("'\\''") } else { out.push(c) }
    }
    out.push('\'');
    out
}

fn read_bytes(path: &str) -> Result<Vec<u8>, String> {
    let fd = sys::openat(path, sys::O_RDONLY | sys::O_CLOEXEC, 0).map_err(|e| oserr("cannot read", path, e))?;
    let mut data = Vec::new();
    let mut buf = [0u8; 4096];
    loop {
        match sys::read(fd, &mut buf) {
            Ok(0) => break,
            Ok(n) => data.extend_from_slice(&buf[..n]),
            Err(4) => {}
            Err(e) => {
                sys::close(fd);
                return Err(oserr("cannot read", path, e));
            }
        }
    }
    sys::close(fd);
    Ok(data)
}

fn read_text(path: &str) -> Result<String, String> {
    String::from_utf8(read_bytes(path)?).map_err(|_| format!("{path}: not a UTF-8 text file"))
}

fn mkdirs(path: &str) {
    let mut cur = String::new();
    let parent = match path.rfind('/') {
        Some(i) => &path[..i],
        None => return,
    };
    for seg in parent.split('/').filter(|s| !s.is_empty()) {
        cur.push('/');
        cur.push_str(seg);
        if sys::stat(&cur).is_err() {
            let _ = sys::mkdir(&cur);
        }
    }
}

fn write_text(path: &str, data: &[u8]) -> Result<(), String> {
    mkdirs(path);
    let fd = sys::openat(path, sys::O_WRONLY | sys::O_CREAT | sys::O_TRUNC | sys::O_CLOEXEC, 0o644)
        .map_err(|e| oserr("cannot write", path, e))?;
    let r = sys::write_all(fd, data);
    sys::close(fd);
    r.map_err(|e| oserr("cannot write", path, e))
}

/// Runs `sh -c cmd` in the project root with stdin from /dev/null, its output on
/// a pipe. (output, exit code, timed out). The whole process group is killed
/// at the timeout.
pub fn run_shell(ctx: &Ctx, cmd: &str, timeout_s: i64) -> Result<(String, i32, bool), String> {
    let (rd, wr) = sys::pipe().map_err(|e| format!("pipe: {}", sys::errno_text(e)))?;
    let sh = sys::cstr("/bin/sh");
    let arg0 = sys::cstr("sh");
    let dash_c = sys::cstr("-c");
    let script = sys::cstr(cmd);
    let argv = [arg0.as_ptr(), dash_c.as_ptr(), script.as_ptr(), core::ptr::null()];
    let pid = sys::fork().map_err(|e| format!("fork: {}", sys::errno_text(e)))?;
    if pid == 0 {
        sys::setpgid(0, 0);
        if let Ok(null) = sys::openat("/dev/null", sys::O_RDONLY, 0) {
            let _ = sys::dup3(null, 0);
        }
        let _ = sys::dup3(wr, 1);
        let _ = sys::dup3(wr, 2);
        let _ = sys::chdir(&ctx.root);
        sys::execve(sh.as_ptr(), argv.as_ptr(), ctx.envp);
        sys::exit(127);
    }
    sys::close(wr);
    let mut out: Vec<u8> = Vec::new();
    let mut total = 0usize;
    let mut timed_out = false;
    let deadline = sys::now_ms() + timeout_s.max(1) * 1000;
    let mut buf = [0u8; 4096];
    loop {
        let left = deadline - sys::now_ms();
        if left <= 0 {
            sys::kill(-pid, sys::SIGKILL);
            sys::kill(pid, sys::SIGKILL);
            timed_out = true;
            break;
        }
        match sys::poll_in(rd, left) {
            Ok(true) => match sys::read(rd, &mut buf) {
                Ok(0) => break,
                Ok(n) => {
                    total += n;
                    if out.len() < MAX_OUT {
                        let take = n.min(MAX_OUT - out.len());
                        out.extend_from_slice(&buf[..take]);
                    }
                }
                Err(4) => {}
                Err(_) => break,
            },
            Ok(false) => {}
            Err(_) => break,
        }
    }
    sys::close(rd);
    let status = sys::wait(pid);
    let rc = if status & 0x7f == 0 { (status >> 8) & 0xff } else { 128 + (status & 0x7f) };
    let mut text = String::from_utf8_lossy(&out).into_owned();
    if total > MAX_OUT {
        text.push_str(&format!("\n...[output truncated at {MAX_OUT} bytes]"));
    }
    Ok((text, rc, timed_out))
}

// ---- tools -------------------------------------------------------------

fn read_file(ctx: &Ctx, a: &Value) -> Out {
    let p = resolve(&ctx.root, a.str("path"));
    let text = read_text(&p)?;
    let body = text.strip_suffix('\n').unwrap_or(&text);
    let lines: Vec<&str> = if text.is_empty() { Vec::new() } else { body.split('\n').collect() };
    let off = a.int("offset").unwrap_or(1).max(1) as usize;
    let lim = a.int("limit").unwrap_or(0);
    let end = if lim <= 0 { lines.len() } else { lines.len().min(off - 1 + lim as usize) };
    let mut out = String::new();
    let mut size = 0usize;
    let mut i = off.saturating_sub(1);
    while i < end {
        let ln = format!("{}\t{}", i + 1, lines[i]);
        size += ln.len() + 1;
        if size > MAX_READ {
            if !out.is_empty() {
                out.push('\n');
            }
            out.push_str(&format!("...[truncated: {} more lines; use offset={}]", lines.len() - i, i + 1));
            break;
        }
        if !out.is_empty() {
            out.push('\n');
        }
        out.push_str(&ln);
        i += 1;
    }
    if out.is_empty() && lines.is_empty() {
        return Ok("(empty file)".to_string());
    }
    Ok(out)
}

fn write_file(ctx: &Ctx, a: &Value) -> Out {
    let p = resolve(&ctx.root, a.str("path"));
    let content = a.str("content").unwrap_or("");
    write_text(&p, content.as_bytes())?;
    Ok(format!("wrote {} bytes to {p}", content.len()))
}

fn edit_file(ctx: &Ctx, a: &Value) -> Out {
    let p = resolve(&ctx.root, a.str("path"));
    let old = a.str("old_string").unwrap_or("");
    let new = a.str("new_string").unwrap_or("");
    if old.is_empty() {
        return Err("old_string must not be empty".to_string());
    }
    let text = read_text(&p)?;
    let n = text.matches(old).count();
    if n == 0 {
        return Err(format!("old_string not found in {p}"));
    }
    let all = a.boolean("replace_all");
    if n > 1 && !all {
        return Err(format!("old_string occurs {n} times in {p}; make it unique or set replace_all"));
    }
    let text = if all { text.replace(old, new) } else { text.replacen(old, new, 1) };
    write_text(&p, text.as_bytes())?;
    Ok(format!("replaced {n} occurrence{} in {p}", if n == 1 { "" } else { "s" }))
}

fn list_dir(ctx: &Ctx, a: &Value) -> Out {
    let p = resolve(&ctx.root, a.str("path"));
    if !sys::is_dir(&p) {
        return Err(format!("not a directory: {p}"));
    }
    let mut names = sys::list_dir(&p).map_err(|e| oserr("cannot list", &p, e))?;
    names.sort();
    let mut out = p.clone();
    out.push('\n');
    if names.is_empty() {
        out.push_str("(empty)");
    }
    for (i, n) in names.iter().enumerate() {
        if i > 0 {
            out.push('\n');
        }
        let name = String::from_utf8_lossy(n);
        let fp = format!("{}/{name}", if p == "/" { "" } else { &p });
        match sys::stat(&fp) {
            Ok((m, _)) if m & sys::S_IFMT == sys::S_IFDIR => out.push_str(&format!("{name}/")),
            Ok((_, size)) => out.push_str(&format!("{name}  {size}")),
            Err(_) => out.push_str(&format!("{name}  ?")),
        }
    }
    Ok(out)
}

fn grep(ctx: &Ctx, a: &Value) -> Out {
    let p = resolve(&ctx.root, a.str("path"));
    let pattern = a.str("pattern").ok_or("pattern is required")?;
    let flags = if a.boolean("ignore_case") { "-rni" } else { "-rn" };
    // BusyBox grep has no --exclude-dir; drop .git hits afterwards.
    let cmd = format!("grep {flags} -e {} {} 2>/dev/null | grep -v '/\\.git/' | head -n 500", shq(pattern), shq(&p));
    let (text, _, _) = run_shell(ctx, &cmd, 60)?;
    Ok(if text.trim().is_empty() { "(no matches)".to_string() } else { text })
}

fn find(ctx: &Ctx, a: &Value) -> Out {
    let p = resolve(&ctx.root, a.str("path"));
    let name = a.str("name").ok_or("name is required")?;
    let cmd = format!("find {} -name .git -prune -o -name {} -print 2>/dev/null | head -n 500", shq(&p), shq(name));
    let (text, _, _) = run_shell(ctx, &cmd, 60)?;
    Ok(if text.trim().is_empty() { "(no matches)".to_string() } else { text })
}

fn bash(ctx: &Ctx, a: &Value) -> Out {
    let cmd = a.str("command").ok_or("command is required")?;
    let timeout = a.int("timeout").filter(|&t| t > 0).unwrap_or(DEFAULT_TIMEOUT);
    let (mut text, rc, timed_out) = run_shell(ctx, cmd, timeout)?;
    if timed_out {
        text.push_str(&format!("\n[timed out after {timeout} s]"));
    }
    if rc != 0 {
        text.push_str(&format!("\n[exit {rc}]"));
    }
    Ok(text)
}

fn delete(ctx: &Ctx, a: &Value) -> Out {
    let p = resolve(&ctx.root, a.str("path"));
    if p == ctx.root {
        return Err("refusing to delete the project root".to_string());
    }
    let flags = if sys::is_dir(&p) { sys::AT_REMOVEDIR } else { 0 };
    sys::unlink(&p, flags).map_err(|e| oserr("cannot delete", &p, e))?;
    Ok(format!("deleted {p}"))
}

/// None: no such tool.
pub fn call(ctx: &Ctx, name: &str, args: &Value) -> Option<Out> {
    Some(match name {
        "read_file" => read_file(ctx, args),
        "write_file" => write_file(ctx, args),
        "edit_file" => edit_file(ctx, args),
        "list_dir" => list_dir(ctx, args),
        "grep" => grep(ctx, args),
        "find" => find(ctx, args),
        "bash" => bash(ctx, args),
        "delete" => delete(ctx, args),
        _ => return None,
    })
}
