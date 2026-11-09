//! Linux system calls for riscv32 (the asm-generic table, 64-bit time only).

use alloc::vec::Vec;
use core::arch::asm;

pub const AT_FDCWD: i32 = -100;
pub const AT_REMOVEDIR: u32 = 0x200;
pub const O_RDONLY: u32 = 0;
pub const O_WRONLY: u32 = 1;
pub const O_CREAT: u32 = 0o100;
pub const O_TRUNC: u32 = 0o1000;
pub const O_DIRECTORY: u32 = 0o200000;
pub const O_CLOEXEC: u32 = 0o2000000;
pub const S_IFMT: u16 = 0xF000;
pub const S_IFDIR: u16 = 0x4000;
pub const SIGKILL: i32 = 9;
pub const SIGCHLD: usize = 17;
pub const POLLIN: i16 = 1;

const NR_GETCWD: usize = 17;
const NR_DUP3: usize = 24;
const NR_MKDIRAT: usize = 34;
const NR_UNLINKAT: usize = 35;
const NR_CHDIR: usize = 49;
const NR_OPENAT: usize = 56;
const NR_CLOSE: usize = 57;
const NR_PIPE2: usize = 59;
const NR_GETDENTS64: usize = 61;
const NR_READ: usize = 63;
const NR_WRITE: usize = 64;
const NR_EXIT_GROUP: usize = 94;
const NR_KILL: usize = 129;
const NR_SETPGID: usize = 154;
const NR_MUNMAP: usize = 215;
const NR_CLONE: usize = 220;
const NR_EXECVE: usize = 221;
const NR_MMAP: usize = 222;
const NR_MADVISE: usize = 233;
const NR_WAIT4: usize = 260;
const NR_STATX: usize = 291;
const NR_CLOCK_GETTIME64: usize = 403;
const NR_PPOLL_TIME64: usize = 414;

pub type Result<T> = core::result::Result<T, i32>;

#[inline(always)]
unsafe fn raw(nr: usize, a0: usize, a1: usize, a2: usize, a3: usize, a4: usize, a5: usize) -> isize {
    let ret: isize;
    unsafe {
        asm!("ecall",
            inlateout("a0") a0 as isize => ret,
            in("a1") a1, in("a2") a2, in("a3") a3, in("a4") a4, in("a5") a5,
            in("a7") nr, options(nostack));
    }
    ret
}

fn check(r: isize) -> Result<usize> {
    if (-4095..0).contains(&r) { Err(-r as i32) } else { Ok(r as usize) }
}

macro_rules! sc {
    ($nr:expr $(, $a:expr)*) => {{
        let args = [$($a as usize,)* 0usize, 0, 0, 0, 0, 0];
        check(unsafe { raw($nr, args[0], args[1], args[2], args[3], args[4], args[5]) })
    }};
}

/// A path as the kernel wants it: NUL-terminated.
pub fn cstr(s: &str) -> Vec<u8> {
    let mut v = Vec::with_capacity(s.len() + 1);
    v.extend_from_slice(s.as_bytes());
    v.push(0);
    v
}

pub fn read(fd: i32, buf: &mut [u8]) -> Result<usize> {
    sc!(NR_READ, fd, buf.as_mut_ptr(), buf.len())
}

pub fn write_all(fd: i32, mut data: &[u8]) -> Result<()> {
    while !data.is_empty() {
        match sc!(NR_WRITE, fd, data.as_ptr(), data.len()) {
            Ok(0) => return Err(5),
            Ok(n) => data = &data[n..],
            Err(4) => {}
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

pub fn openat(path: &str, flags: u32, mode: u32) -> Result<i32> {
    let p = cstr(path);
    sc!(NR_OPENAT, AT_FDCWD, p.as_ptr(), flags, mode).map(|fd| fd as i32)
}

pub fn close(fd: i32) {
    let _ = sc!(NR_CLOSE, fd);
}

/// The file's mode and size, through statx (rv32 has no fstat/newfstatat).
pub fn stat(path: &str) -> Result<(u16, u64)> {
    let p = cstr(path);
    let mut buf = [0u8; 256];
    sc!(NR_STATX, AT_FDCWD, p.as_ptr(), 0, 0x7ff, buf.as_mut_ptr())?;
    let mode = u16::from_le_bytes([buf[0x1c], buf[0x1d]]);
    let size = u64::from_le_bytes(buf[0x28..0x30].try_into().unwrap());
    Ok((mode, size))
}

pub fn is_dir(path: &str) -> bool {
    matches!(stat(path), Ok((m, _)) if m & S_IFMT == S_IFDIR)
}

pub fn mkdir(path: &str) -> Result<()> {
    let p = cstr(path);
    sc!(NR_MKDIRAT, AT_FDCWD, p.as_ptr(), 0o755).map(|_| ())
}

pub fn unlink(path: &str, flags: u32) -> Result<()> {
    let p = cstr(path);
    sc!(NR_UNLINKAT, AT_FDCWD, p.as_ptr(), flags).map(|_| ())
}

pub fn chdir(path: &str) -> Result<()> {
    let p = cstr(path);
    sc!(NR_CHDIR, p.as_ptr()).map(|_| ())
}

pub fn getcwd() -> Result<Vec<u8>> {
    let mut buf = alloc::vec![0u8; 4096];
    let n = sc!(NR_GETCWD, buf.as_mut_ptr(), buf.len())?;
    buf.truncate(n.saturating_sub(1));
    Ok(buf)
}

/// Names in a directory, in disk order.
pub fn list_dir(path: &str) -> Result<Vec<Vec<u8>>> {
    let fd = openat(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0)?;
    let mut names = Vec::new();
    let mut buf = alloc::vec![0u8; 4096];
    loop {
        let n = match sc!(NR_GETDENTS64, fd, buf.as_mut_ptr(), buf.len()) {
            Ok(0) => break,
            Ok(n) => n,
            Err(e) => {
                close(fd);
                return Err(e);
            }
        };
        let mut off = 0;
        while off < n {
            // linux_dirent64: ino u64, off i64, reclen u16, type u8, name...
            let reclen = u16::from_le_bytes([buf[off + 16], buf[off + 17]]) as usize;
            let name = &buf[off + 19..off + reclen];
            let end = name.iter().position(|&b| b == 0).unwrap_or(name.len());
            let name = &name[..end];
            if name != b"." && name != b".." {
                names.push(name.to_vec());
            }
            off += reclen;
        }
    }
    close(fd);
    Ok(names)
}

pub fn exit(code: i32) -> ! {
    loop {
        let _ = sc!(NR_EXIT_GROUP, code);
    }
}

pub fn mmap(len: usize) -> Result<*mut u8> {
    // PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS
    sc!(NR_MMAP, 0, len, 3, 0x22, -1isize, 0).map(|p| p as *mut u8)
}

pub fn munmap(p: *mut u8, len: usize) {
    let _ = sc!(NR_MUNMAP, p, len);
}

/// MADV_DONTNEED: give the pages back, keep the mapping.
pub fn release(p: *mut u8, len: usize) {
    let _ = sc!(NR_MADVISE, p, len, 4);
}

pub fn pipe() -> Result<(i32, i32)> {
    let mut fds = [0i32; 2];
    sc!(NR_PIPE2, fds.as_mut_ptr(), O_CLOEXEC)?;
    Ok((fds[0], fds[1]))
}

pub fn dup3(old: i32, new: i32) -> Result<()> {
    sc!(NR_DUP3, old, new, 0).map(|_| ())
}

/// fork(): clone with only SIGCHLD, no new stack.
pub fn fork() -> Result<i32> {
    sc!(NR_CLONE, SIGCHLD, 0, 0, 0, 0).map(|p| p as i32)
}

pub fn setpgid(pid: i32, pgid: i32) {
    let _ = sc!(NR_SETPGID, pid, pgid);
}

pub fn execve(path: *const u8, argv: *const *const u8, envp: *const *const u8) -> i32 {
    match sc!(NR_EXECVE, path, argv, envp) {
        Err(e) => e,
        Ok(_) => 0,
    }
}

pub fn kill(pid: i32, sig: i32) {
    let _ = sc!(NR_KILL, pid, sig);
}

/// Waits for the child; the raw wait status.
pub fn wait(pid: i32) -> i32 {
    let mut status = 0i32;
    loop {
        match sc!(NR_WAIT4, pid, &mut status as *mut i32, 0, 0) {
            Err(4) => {}
            _ => return status,
        }
    }
}

/// True when fd is readable before the timeout (ms); false on timeout.
pub fn poll_in(fd: i32, timeout_ms: i64) -> Result<bool> {
    #[repr(C)]
    struct PollFd {
        fd: i32,
        events: i16,
        revents: i16,
    }
    let mut pfd = PollFd { fd, events: POLLIN, revents: 0 };
    let ts: [i64; 2] = [timeout_ms / 1000, (timeout_ms % 1000) * 1_000_000];
    loop {
        match sc!(NR_PPOLL_TIME64, &mut pfd as *mut PollFd, 1, ts.as_ptr(), 0, 8) {
            Ok(0) => return Ok(false),
            Ok(_) => return Ok(true),
            Err(4) => {}
            Err(e) => return Err(e),
        }
    }
}

/// CLOCK_MONOTONIC in milliseconds.
pub fn now_ms() -> i64 {
    let mut ts = [0i64; 2];
    let _ = sc!(NR_CLOCK_GETTIME64, 1, ts.as_mut_ptr());
    ts[0] * 1000 + ts[1] / 1_000_000
}

pub fn errno_text(e: i32) -> &'static str {
    match e {
        1 => "operation not permitted",
        2 => "no such file or directory",
        5 => "I/O error",
        9 => "bad file descriptor",
        12 => "out of memory",
        13 => "permission denied",
        17 => "file exists",
        20 => "not a directory",
        21 => "is a directory",
        22 => "invalid argument",
        24 => "too many open files",
        26 => "text file busy",
        27 => "file too large",
        28 => "no space left on device",
        30 => "read-only file system",
        36 => "file name too long",
        39 => "directory not empty",
        _ => "error",
    }
}
