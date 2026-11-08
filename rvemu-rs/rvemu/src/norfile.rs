//! The NOR flash device's backing on the Mac (`-F FILE`): a file, made
//! 1 MB of 0xFF (blank flash) when it doesn't exist, written through on
//! every program and erase, so it persists across runs as the board's
//! flash region does.
//!
//! `RVEMU_NOR_CUT=N` simulates a power cut: the N-th program or erase is
//! applied only halfway, and the emulator exits at once.

use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};

use rvcore::nor::{ERASESIZE, NorFlash};

pub const SIZE: u32 = 1 << 20;

pub struct FileNor {
    file: File,
    data: Vec<u8>,
    ops: u64,
    cut_at: u64,
}

impl FileNor {
    pub fn open(path: &str) -> Result<FileNor, String> {
        let err = |e: std::io::Error| format!("{path}: {e}");
        let mut file = OpenOptions::new().read(true).write(true).create(true).truncate(false).open(path).map_err(err)?;
        let mut data = Vec::new();
        file.read_to_end(&mut data).map_err(err)?;
        if data.len() != SIZE as usize {
            data.resize(SIZE as usize, 0xFF);
            file.set_len(0).map_err(err)?;
            file.seek(SeekFrom::Start(0)).map_err(err)?;
            file.write_all(&data).map_err(err)?;
        }
        let cut_at = std::env::var("RVEMU_NOR_CUT").ok().and_then(|v| v.parse().ok()).unwrap_or(0);
        Ok(FileNor { file, data, ops: 0, cut_at })
    }

    fn flush(&mut self, off: u32, len: usize) -> bool {
        let range = off as usize..off as usize + len;
        self.file.seek(SeekFrom::Start(u64::from(off))).is_ok() && self.file.write_all(&self.data[range]).is_ok()
    }

    /// Count an operation; true when it is the one the power cut hits.
    fn cut(&mut self) -> bool {
        self.ops += 1;
        self.cut_at != 0 && self.ops == self.cut_at
    }

    fn die(&mut self) -> ! {
        let _ = self.file.sync_all();
        eprintln!("\n[rvemu] RVEMU_NOR_CUT: power cut at flash operation {}", self.ops);
        std::process::exit(3);
    }
}

impl NorFlash for FileNor {
    fn size(&self) -> u32 {
        SIZE
    }
    fn read(&mut self, off: u32, buf: &mut [u8]) -> bool {
        buf.copy_from_slice(&self.data[off as usize..off as usize + buf.len()]);
        true
    }
    fn program(&mut self, off: u32, data: &[u8]) -> bool {
        let cut = self.cut();
        let n = if cut { data.len() / 2 } else { data.len() };
        for (d, s) in self.data[off as usize..].iter_mut().zip(&data[..n]) {
            *d &= *s;
        }
        let ok = self.flush(off, data.len());
        if cut {
            self.die();
        }
        ok
    }
    fn erase(&mut self, off: u32) -> bool {
        let cut = self.cut();
        let n = if cut { ERASESIZE as usize / 2 } else { ERASESIZE as usize };
        self.data[off as usize..off as usize + n].fill(0xFF);
        let ok = self.flush(off, ERASESIZE as usize);
        if cut {
            self.die();
        }
        ok
    }
}
