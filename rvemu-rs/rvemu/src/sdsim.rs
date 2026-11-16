//! A simulated SD card on the Mac (`-s FILE`): an SDHC card in SPI mode,
//! byte by byte on the bus, backed by FILE, for `rvcore::sdcard` (the same
//! driver the board runs). The card is in while FILE exists: rename it away
//! and back to take the card out and put it in again (looked at every half
//! second).
//!
//! It checks what the driver must get right: CMD0 before anything else, the
//! CRC7 of every command once CRC checking is on (and of CMD0 and CMD8
//! always), the CRC16 of every written block, tokens and the data response,
//! CMD12 ending a multi-block read, and DO held low while it is busy after a
//! write (`BUSY_BYTES`), which the driver must wait out before its next
//! command. `RVEMU_SD_FLIP=N` corrupts one bit of every Nth data byte read,
//! so the CRC check and the retries can be watched (`[sdsim]` lines).

use std::collections::VecDeque;
use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::PathBuf;
use std::time::{Duration, Instant};

use rvcore::sdcard::{SdBus, crc7, crc16};

/// Bytes of busy (DO low) after each written block, and after CMD12.
const BUSY_BYTES: u32 = 40;
/// Bytes of 0xFF before a data block's token (Nac).
const NAC: usize = 3;
/// ACMD41 answers "still idle" this many times first.
const ACMD41_IDLE: u32 = 3;

#[derive(PartialEq)]
enum Phase {
    /// Powered, not yet in SPI mode: only CMD0 (with chip select on) counts.
    Native,
    /// SPI mode, idle state (CMD0 seen, ACMD41 not finished).
    Idle,
    Ready,
}

enum Rx {
    /// Waiting for, or collecting, a command frame.
    Command,
    /// CMD24/CMD25: a token, 512 bytes, the CRC (`multi`: until 0xFD).
    Write { lba: u64, multi: bool, buf: Vec<u8> },
}

pub struct SimCard {
    path: PathBuf,
    file: Option<File>,
    sectors: u64,
    next_look: Instant,
    selected: bool,
    phase: Phase,
    crc_on: bool,
    app: bool,
    acmd41: u32,
    rx: Rx,
    frame: Vec<u8>,
    out: VecDeque<u8>,
    busy: u32,
    /// CMD18 in progress: the next block to send.
    reading: Option<u64>,
    hz: u32,
    /// Bus time so far (bits clocked, at the rate in force).
    bus_ns: u64,
    start: Instant,
    flip: u64,
    nread: u64,
}

impl SimCard {
    pub fn new(path: &str) -> Self {
        let flip = std::env::var("RVEMU_SD_FLIP").ok().and_then(|v| v.parse().ok()).unwrap_or(0);
        let mut c = SimCard {
            path: PathBuf::from(path),
            file: None,
            sectors: 0,
            next_look: Instant::now(),
            selected: false,
            phase: Phase::Native,
            crc_on: false,
            app: false,
            acmd41: 0,
            rx: Rx::Command,
            frame: Vec::new(),
            out: VecDeque::new(),
            busy: 0,
            reading: None,
            hz: 400_000,
            bus_ns: 0,
            start: Instant::now(),
            flip,
            nread: 0,
        };
        c.look();
        c
    }

    /// Card in or out (FILE exists or not).
    fn look(&mut self) {
        let now = Instant::now();
        if now < self.next_look {
            return;
        }
        self.next_look = now + Duration::from_millis(500);
        let exists = self.path.exists();
        if exists && self.file.is_none() {
            if let Ok(f) = OpenOptions::new().read(true).write(true).open(&self.path) {
                let len = f.metadata().map(|m| m.len()).unwrap_or(0);
                // CSD v2 counts 512 KiB units
                self.sectors = len / 512 / 1024 * 1024;
                self.file = Some(f);
                self.reset();
                eprintln!("\r\n[sdsim] card in: {} ({} sectors)\r", self.path.display(), self.sectors);
            }
        } else if !exists && self.file.is_some() {
            self.file = None;
            eprintln!("\r\n[sdsim] card out\r");
        }
    }

    fn reset(&mut self) {
        self.phase = Phase::Native;
        self.crc_on = false;
        self.app = false;
        self.acmd41 = 0;
        self.rx = Rx::Command;
        self.frame.clear();
        self.out.clear();
        self.busy = 0;
        self.reading = None;
    }

    fn r1(&self) -> u8 {
        if self.phase == Phase::Idle { 1 } else { 0 }
    }

    fn respond(&mut self, bytes: &[u8]) {
        self.out.push_back(0xFF); // Ncr
        self.out.extend(bytes);
    }

    fn block(&mut self, data: &[u8]) {
        self.out.extend(std::iter::repeat_n(0xFF, NAC));
        self.out.push_back(0xFE);
        let start = self.out.len();
        self.out.extend(data);
        if self.flip != 0 {
            for i in 0..data.len() {
                self.nread += 1;
                if self.nread.is_multiple_of(self.flip) {
                    self.out[start + i] ^= 0x10;
                    eprintln!("\r\n[sdsim] flipped a bit in a block\r");
                }
            }
        }
        self.out.extend(crc16(data).to_be_bytes());
    }

    fn sector(&mut self, lba: u64) -> Option<Vec<u8>> {
        let f = self.file.as_mut()?;
        let mut b = vec![0; 512];
        f.seek(SeekFrom::Start(lba * 512)).ok()?;
        f.read_exact(&mut b).ok()?;
        Some(b)
    }

    fn csd(&self) -> [u8; 16] {
        let c = (self.sectors / 1024).saturating_sub(1);
        let mut csd = [0x40, 0x0E, 0x00, 0x32, 0x5B, 0x59, 0x00, (c >> 16) as u8 & 0x3F, (c >> 8) as u8, c as u8, 0x7F, 0x80, 0x0A, 0x40, 0x00, 0];
        csd[15] = crc7(&csd[..15]) << 1 | 1;
        csd
    }

    fn command(&mut self) {
        let f: [u8; 6] = self.frame[..6].try_into().unwrap();
        self.frame.clear();
        let idx = f[0] & 0x3F;
        let arg = u32::from_be_bytes([f[1], f[2], f[3], f[4]]);
        let app = std::mem::take(&mut self.app);
        if self.phase == Phase::Native && idx != 0 {
            return; // not in SPI mode yet: no answer
        }
        let crc_ok = crc7(&f[..5]) << 1 | 1 == f[5];
        if (self.crc_on || idx == 0 || idx == 8) && !crc_ok {
            self.respond(&[self.r1() | 0x08]);
            return;
        }
        match (app, idx) {
            (_, 0) => {
                self.reset();
                self.phase = Phase::Idle;
                self.respond(&[1]);
            }
            (_, 8) => self.respond(&[self.r1(), 0, 0, (arg >> 8) as u8 & 0x0F, arg as u8]),
            (_, 59) => {
                self.crc_on = arg & 1 != 0;
                self.respond(&[self.r1()]);
            }
            (_, 55) => {
                self.app = true;
                self.respond(&[self.r1()]);
            }
            (true, 41) => {
                self.acmd41 += 1;
                if self.acmd41 > ACMD41_IDLE {
                    self.phase = Phase::Ready;
                }
                self.respond(&[self.r1()]);
            }
            (_, 58) => self.respond(&[self.r1(), 0xC0, 0xFF, 0x80, 0x00]),
            _ if self.phase != Phase::Ready => self.respond(&[self.r1() | 0x04]),
            (_, 9) => {
                self.respond(&[0]);
                let csd = self.csd();
                self.block(&csd);
            }
            (_, 10) => {
                self.respond(&[0]);
                let mut cid = *b"\x03SDrvsim\x10\x12\x34\x56\x78\x01\x6A\x00";
                cid[15] = crc7(&cid[..15]) << 1 | 1;
                self.block(&cid);
            }
            (_, 13) => self.respond(&[0, 0]),
            (_, 16) => self.respond(&[if arg == 512 { 0 } else { 0x40 }]),
            (_, 12) => {
                // stop: drop what's queued; stuff byte, R1, then busy
                self.reading = None;
                self.out.clear();
                self.out.extend([0xFF, 0x00]);
                self.busy = BUSY_BYTES;
            }
            (_, 17) | (_, 18) => {
                let lba = u64::from(arg);
                match self.sector(lba).filter(|_| lba < self.sectors) {
                    Some(d) => {
                        self.respond(&[0]);
                        self.block(&d);
                        if idx == 18 {
                            self.reading = Some(lba + 1);
                        }
                    }
                    None => self.respond(&[0x20]), // address error
                }
            }
            (_, 24) | (_, 25) => {
                let lba = u64::from(arg);
                if lba >= self.sectors {
                    self.respond(&[0x20]);
                } else {
                    self.respond(&[0]);
                    self.rx = Rx::Write { lba, multi: idx == 25, buf: Vec::new() };
                }
            }
            _ => self.respond(&[0x04]),
        }
    }

    /// A byte the driver clocks out, and what goes back.
    fn byte(&mut self, b: u8) -> u8 {
        if !self.selected || self.file.is_none() {
            return 0xFF;
        }
        // what goes back was decided before this byte arrived
        let out = if let Some(o) = self.out.pop_front() {
            o
        } else if self.busy > 0 {
            self.busy -= 1;
            0x00
        } else {
            0xFF
        };
        if self.out.is_empty()
            && self.busy == 0
            && let Some(lba) = self.reading
        {
            match self.sector(lba).filter(|_| lba < self.sectors) {
                Some(d) => {
                    self.block(&d);
                    self.reading = Some(lba + 1);
                }
                None => self.reading = None,
            }
        }
        match &mut self.rx {
            Rx::Write { lba, multi, buf } => {
                if buf.is_empty() {
                    match b {
                        0xFE if !*multi => buf.push(b),
                        0xFC if *multi => buf.push(b),
                        0xFD if *multi => {
                            self.rx = Rx::Command;
                            self.out.push_back(0xFF);
                            self.busy = BUSY_BYTES;
                        }
                        _ => {}
                    }
                } else {
                    buf.push(b);
                    if buf.len() == 1 + 512 + 2 {
                        let (lba0, multi0) = (*lba, *multi);
                        let data = buf[1..513].to_vec();
                        let ok = crc16(&data).to_be_bytes() == [buf[513], buf[514]];
                        if ok {
                            if let Some(f) = self.file.as_mut() {
                                let _ = f.seek(SeekFrom::Start(lba0 * 512)).and_then(|_| f.write_all(&data));
                            }
                            self.out.push_back(0xE5); // accepted
                            self.busy = BUSY_BYTES;
                        } else {
                            eprintln!("\r\n[sdsim] bad CRC16 on a written block\r");
                            self.out.push_back(0xEB); // CRC error
                        }
                        if multi0 && ok && lba0 + 1 < self.sectors {
                            self.rx = Rx::Write { lba: lba0 + 1, multi: true, buf: Vec::new() };
                        } else {
                            self.rx = Rx::Command;
                        }
                    }
                }
            }
            Rx::Command => {
                if self.frame.is_empty() {
                    if b & 0xC0 == 0x40 {
                        self.frame.push(b);
                    }
                } else {
                    self.frame.push(b);
                    if self.frame.len() == 6 {
                        self.command();
                    }
                }
            }
        }
        out
    }
}

impl SdBus for SimCard {
    fn set_hz(&mut self, hz: u32) {
        self.hz = hz.max(1);
    }
    fn select(&mut self, on: bool) {
        self.look();
        self.selected = on;
        if !on {
            self.frame.clear();
        }
    }
    fn transfer(&mut self, buf: &mut [u8]) {
        self.bus_ns += buf.len() as u64 * 8_000_000_000 / u64::from(self.hz);
        for b in buf.iter_mut() {
            *b = self.byte(*b);
        }
    }
    fn now_us(&mut self) -> u64 {
        // real time keeps the insertion poll going; bus time makes busy
        // waits and timeouts count bytes, as on the wire
        self.start.elapsed().as_micros() as u64 + self.bus_ns / 1000
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rvcore::sdcard::{DEFAULT_HZ, SdCard};
    use rvcore::virtio::Disk;

    fn image(name: &str, mib: u64) -> PathBuf {
        let p = std::env::temp_dir().join(format!("sdsim-{}-{name}.img", std::process::id()));
        let f = File::create(&p).unwrap();
        f.set_len(mib << 20).unwrap();
        p
    }

    #[test]
    fn read_write_retry_hotplug() {
        let p = image("rw", 2);
        let mut card = SdCard::new(SimCard::new(p.to_str().unwrap()), DEFAULT_HZ);
        assert!(card.start());
        assert_eq!(card.sectors(), 4096);
        // single and multi-block writes, read back both ways
        let data: Vec<u8> = (0..8 * 512).map(|i| (i * 7 + i / 512) as u8).collect();
        assert!(card.write(100, &data[..512]));
        assert!(card.write(200, &data));
        let mut b = vec![0; 8 * 512];
        assert!(card.read(200, &mut b));
        assert_eq!(b, data);
        let mut one = vec![0; 512];
        assert!(card.read(100, &mut one));
        assert_eq!(one, data[..512]);
        assert!(card.read(203, &mut one));
        assert_eq!(one, data[3 * 512..4 * 512]);
        assert!(!card.read(4095, &mut b), "past the end");
        // one request in three segments, out and back
        let (x, y, z) = (vec![0x11u8; 1024], vec![0x22u8; 4096], vec![0x33u8; 512]);
        assert!(card.write_segs(1000, &[&x, &y, &z]));
        let (mut s1, mut s2, mut s3) = (vec![0u8; 512], vec![0u8; 4096], vec![0u8; 1024]);
        assert!(card.read_segs(1000, &mut [&mut s1, &mut s2, &mut s3]));
        assert_eq!(
            (s1[0], s1[511], s2[0], s2[512], s2[4095], s3[0], s3[511], s3[512]),
            (0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22, 0x33)
        );
        assert!(!card.write_segs(1000, &[&x[..100]]), "not whole sectors");
        assert_eq!(card.stats.errors, 0);
        // a corrupted byte: the CRC catches it and the retry gets it right
        card.bus.flip = 10_000;
        card.bus.nread = 9_900;
        assert!(card.read(200, &mut b));
        assert_eq!(b, data);
        assert!(card.stats.crc_errors > 0);
        card.bus.flip = 0;
        // out: the next poll drops it (the sim looks every 500 ms)
        let q = p.with_extension("out");
        std::fs::rename(&p, &q).unwrap();
        std::thread::sleep(Duration::from_millis(600));
        card.bus.next_look = Instant::now();
        card.bus.look();
        let deadline = Instant::now() + Duration::from_secs(5);
        while card.sectors() != 0 && Instant::now() < deadline {
            card.poll();
            std::thread::sleep(Duration::from_millis(50));
        }
        assert_eq!(card.sectors(), 0);
        assert!(!card.read(0, &mut one));
        // and in again: the data is still there
        std::fs::rename(&q, &p).unwrap();
        let deadline = Instant::now() + Duration::from_secs(5);
        while card.sectors() == 0 && Instant::now() < deadline {
            card.poll();
            std::thread::sleep(Duration::from_millis(50));
        }
        assert_eq!(card.sectors(), 4096);
        assert!(card.read(200, &mut b));
        assert_eq!(b, data);
        let _ = std::fs::remove_file(&p);
    }
}
