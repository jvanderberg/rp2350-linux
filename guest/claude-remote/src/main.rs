//! claude-guest-mcp: an MCP (Model Context Protocol) tool server over stdio.
//!
//! Runs on the device that owns the files. Claude Code, running elsewhere,
//! talks to it through an ssh session and gets tools to read, write, edit,
//! list and search files and to run shell commands here. Newline-delimited
//! JSON-RPC 2.0 on stdin/stdout.
//!
//!   claude-guest-mcp [ROOT]       (default ROOT: current directory)
//!
//! No libc: a static rv32 Linux executable on raw system calls, so that the
//! guest's few megabytes stay free for the work Claude asks for.

#![no_std]
#![no_main]

extern crate alloc;

mod heap;
mod json;
mod sys;
mod tools;

use alloc::string::String;
use alloc::vec::Vec;
use core::arch::global_asm;
use json::Value;

global_asm!(
    r#"
.section .text._start,"ax"
.globl _start
_start:
    mv a0, sp
    andi sp, sp, -16
    call start
1:  j 1b
"#
);

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    let _ = sys::write_all(2, b"claude-guest-mcp: internal error\n");
    sys::exit(70)
}

fn send(out: &mut Vec<u8>) {
    out.push(b'\n');
    if sys::write_all(1, out).is_err() {
        sys::exit(0); // the other end is gone
    }
    out.clear();
}

fn begin(out: &mut Vec<u8>, id: &Value, key: &str) {
    out.extend_from_slice(b"{\"jsonrpc\":\"2.0\",\"id\":");
    json::write(id, out);
    out.extend_from_slice(b",\"");
    out.extend_from_slice(key.as_bytes());
    out.extend_from_slice(b"\":");
}

fn reply_raw(out: &mut Vec<u8>, id: &Value, result_json: &[u8]) {
    begin(out, id, "result");
    out.extend_from_slice(result_json);
    out.push(b'}');
    send(out);
}

fn error(out: &mut Vec<u8>, id: &Value, code: i32, msg: &str) {
    begin(out, id, "error");
    out.extend_from_slice(alloc::format!("{{\"code\":{code},\"message\":").as_bytes());
    json::write_str(msg, out);
    out.extend_from_slice(b"}}");
    send(out);
}

fn handle(ctx: &tools::Ctx, req: &Value, out: &mut Vec<u8>) {
    let method = req.str("method").unwrap_or("");
    let id = req.get("id").cloned().unwrap_or(Value::Null);
    let empty = Value::Obj(Vec::new());
    let params = req.get("params").unwrap_or(&empty);
    match method {
        "initialize" => {
            let mut r = Vec::new();
            r.extend_from_slice(b"{\"protocolVersion\":");
            json::write_str(params.str("protocolVersion").unwrap_or("2024-11-05"), &mut r);
            r.extend_from_slice(b",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"claude-guest-mcp\",\"version\":\"0.2\"},\"instructions\":");
            json::write_str(
                &alloc::format!(
                    "The project files live on this device at {}. Use these tools for all file and shell operations; the files are not reachable any other way.",
                    ctx.root
                ),
                &mut r,
            );
            r.push(b'}');
            reply_raw(out, &id, &r);
        }
        "ping" => reply_raw(out, &id, b"{}"),
        "tools/list" => {
            let mut r = Vec::new();
            r.extend_from_slice(b"{\"tools\":");
            r.extend_from_slice(tools::TOOLS_JSON.as_bytes());
            r.push(b'}');
            reply_raw(out, &id, &r);
        }
        "tools/call" => {
            let name = params.str("name").unwrap_or("");
            let args = params.get("arguments").unwrap_or(&empty);
            match tools::call(ctx, name, args) {
                None => error(out, &id, -32601, &alloc::format!("unknown tool: {name}")),
                Some(res) => {
                    let (text, is_err) = match res {
                        Ok(t) => (t, false),
                        Err(e) => (alloc::format!("error: {e}"), true),
                    };
                    let mut r = Vec::new();
                    r.extend_from_slice(b"{\"content\":[{\"type\":\"text\",\"text\":");
                    json::write_str(&text, &mut r);
                    r.extend_from_slice(if is_err { b"}],\"isError\":true}" } else { b"}]}" });
                    reply_raw(out, &id, &r);
                }
            }
        }
        _ if matches!(id, Value::Null) => {} // a notification
        _ => error(out, &id, -32601, &alloc::format!("method not found: {method}")),
    }
}

#[unsafe(no_mangle)]
extern "C" fn start(sp: *const usize) -> ! {
    heap::Heap::init();
    let argc = unsafe { *sp };
    let argv = unsafe { sp.add(1) as *const *const u8 };
    let envp = unsafe { argv.add(argc + 1) };
    // The root and the line buffer outlive requests: both big enough to be
    // mappings of their own, not arena blocks (see heap.rs).
    let mut root = String::with_capacity(4096);
    let raw: Vec<u8> = if argc > 1 {
        let p = unsafe { *argv.add(1) };
        let mut n = 0;
        while unsafe { *p.add(n) } != 0 {
            n += 1;
        }
        unsafe { core::slice::from_raw_parts(p, n) }.to_vec()
    } else {
        sys::getcwd().unwrap_or_else(|_| b"/".to_vec())
    };
    let raw = String::from_utf8_lossy(&raw).into_owned();
    let cwd = String::from_utf8_lossy(&sys::getcwd().unwrap_or_else(|_| b"/".to_vec())).into_owned();
    root.push_str(&tools::resolve(&cwd, Some(&raw)));
    let ctx = tools::Ctx { root, envp };

    let mut line: Vec<u8> = Vec::with_capacity(16384);
    let mut out: Vec<u8> = Vec::with_capacity(16384);
    let mut buf = [0u8; 4096];
    let mut pending: Vec<u8> = Vec::with_capacity(4096);
    loop {
        // one request per line
        let nl = loop {
            if let Some(i) = pending.iter().position(|&b| b == b'\n') {
                break Some(i);
            }
            match sys::read(0, &mut buf) {
                Ok(0) => break None,
                Ok(n) => pending.extend_from_slice(&buf[..n]),
                Err(4) => {}
                Err(_) => break None,
            }
        };
        let Some(nl) = nl else { break };
        line.clear();
        line.extend_from_slice(&pending[..nl]);
        pending.drain(..=nl);
        let text = &line[..];
        if text.iter().all(|b| matches!(b, b' ' | b'\t' | b'\r')) {
            continue;
        }
        match json::parse(text) {
            Some(req) => handle(&ctx, &req, &mut out),
            None => error(&mut out, &Value::Null, -32700, "parse error"),
        }
        heap::Heap::reset();
    }
    sys::exit(0)
}
