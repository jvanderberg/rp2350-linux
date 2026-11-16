//! An SD card on an SPI bus, as a `Disk` for a writable virtio-blk device:
//! the SD protocol runs in the host, not in the guest, so a 4 KiB request
//! is one multi-block command, CRC16 is computed natively, and the guest
//! spends no instructions on SPI framing.
//!
//! - **Commands.** SPI mode with CRC checking on (CMD59): every command
//!   carries its CRC7 and every data block its CRC16, both ways. A request
//!   of more than one sector is one CMD18 (read) or CMD25 (write).
//! - **Deferred busy.** After a write the card holds DO low while it
//!   programs. That wait is left to the next command (which waits for DO to
//!   go high first), so the guest runs while the card programs.
//! - **Errors.** A failed command or a CRC mismatch is retried, after a
//!   status check (CMD13) and, failing that, a fresh initialisation of the
//!   card. A card that can't be brought back is gone (`sectors` is 0).
//! - **Insertion and removal.** `poll`, called while the guest idles, looks
//!   for a card about once a second: a new card is initialised, and a card
//!   that stops answering is dropped. The card's CID tells a re-inserted
//!   card from a different one. A change in capacity is what the guest's
//!   virtio driver sees (a config-change interrupt).
//! - **Capacity.** SDSC (byte addressed, CSD v1) and SDHC/SDXC (block
//!   addressed, CSD v2) cards; SDUC is not supported.

/// The bus the card is on: one chip select, mode 0, MSB first.
pub trait SdBus {
    /// Clock rate for what follows: `hz` or the nearest rate below.
    fn set_hz(&mut self, hz: u32);
    /// Chip select (active low on the wire) on or off.
    fn select(&mut self, on: bool);
    /// Full duplex: send `buf` and replace it with what came back.
    fn transfer(&mut self, buf: &mut [u8]);
    /// Send `tx`; what comes back is dropped.
    fn send(&mut self, tx: &[u8]) {
        let mut b = [0u8; 64];
        for c in tx.chunks(b.len()) {
            b[..c.len()].copy_from_slice(c);
            self.transfer(&mut b[..c.len()]);
        }
    }
    /// Send 0xFF for each byte of `rx`, keeping what comes back.
    fn recv(&mut self, rx: &mut [u8]) {
        rx.fill(0xFF);
        self.transfer(rx);
    }
    /// Send one byte, return the one that came back.
    fn xchg(&mut self, b: u8) -> u8 {
        let mut r = [b];
        self.transfer(&mut r);
        r[0]
    }
    /// A monotonic microsecond clock, for timeouts and the insertion poll.
    fn now_us(&mut self) -> u64;
}

/// Clock while the card is being initialised (the spec's limit is 400 kHz).
pub const INIT_HZ: u32 = 400_000;
/// Default clock once the card is up: the default-speed mode's limit.
pub const DEFAULT_HZ: u32 = 25_000_000;
/// High-speed mode's limit (CMD6, function group 1), for a card that
/// switches; the bus runs at the nearest rate below.
pub const HIGH_HZ: u32 = 50_000_000;
/// CRC errors at a clock above `DEFAULT_HZ` before the driver settles for
/// `DEFAULT_HZ` (the wiring can't take the faster clock).
const FAST_CRC_LIMIT: u32 = 3;
/// How long the card may take to leave idle state (ACMD41).
const INIT_US: u64 = 1_000_000;
/// How long a read's data token may take (Nac for SDHC is at most 100 ms).
const READ_US: u64 = 100_000;
/// How long the card may stay busy after a write (250 ms for SDHC; a card
/// doing its own housekeeping can take longer).
const BUSY_US: u64 = 500_000;
/// How often `poll` looks for a card, or checks the one there.
const POLL_US: u64 = 1_000_000;
/// Attempts at a request (the first try and the retries).
const TRIES: u32 = 3;
/// Pages (data segments) per request: one multi-block command for all of
/// them, so the card's access time is paid once. The guest waits while a
/// request runs, which caps it: 64 KiB is about 22 ms at 25 MHz.
pub const SEGMENTS: u32 = 16;

const R1_IDLE: u8 = 0x01;
const R1_ILLEGAL: u8 = 0x04;
const TOKEN_START: u8 = 0xFE;
const TOKEN_START_MULTI: u8 = 0xFC;
const TOKEN_STOP: u8 = 0xFD;
/// The data response's status field: accepted.
const DATA_ACCEPTED: u8 = 0x05;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SdError {
    /// No R1 came back: no card, or it isn't in SPI mode.
    NoResponse,
    /// The command was rejected (its R1).
    R1(u8),
    /// The card stayed busy, or sent no data token, for too long.
    Timeout,
    /// A data block's CRC16 didn't match.
    Crc,
    /// The card sent an error token instead of data.
    Token(u8),
    /// The card refused a written block (its data response).
    Write(u8),
    /// CMD8's echo was wrong, or ACMD41 never finished.
    Init,
    /// A card this driver can't use (SDUC, or a bad CSD).
    Unsupported,
}

/// Counters since boot.
#[derive(Clone, Copy, Debug, Default)]
pub struct SdStats {
    pub reads: u32,
    pub writes: u32,
    pub sectors_read: u64,
    pub sectors_written: u64,
    /// Requests that failed a try (and were retried, or failed).
    pub errors: u32,
    pub crc_errors: u32,
    /// CRC errors at a clock above `DEFAULT_HZ`.
    pub fast_crc_errors: u32,
    /// Requests that failed every try.
    pub failed: u32,
    /// Cards initialised (insertions, and re-initialisations after errors).
    pub inits: u32,
    /// Longest wait for the card to stop being busy, in microseconds.
    pub busy_us_max: u32,
    pub last_error: Option<SdError>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum State {
    /// No card, or none that initialised.
    Absent,
    Ready,
}

/// The card's identity and size, from its registers.
#[derive(Clone, Copy, PartialEq, Eq)]
struct CardInfo {
    sectors: u64,
    /// Block addressing (SDHC/SDXC) rather than byte addressing (SDSC).
    hc: bool,
    cid: [u8; 16],
}

pub struct SdCard<B: SdBus> {
    pub bus: B,
    /// The clock asked for once the card is up.
    hz: u32,
    /// The clock in use: `hz`, or `DEFAULT_HZ` for a card without
    /// high-speed mode or wiring that gave CRC errors at `hz`.
    run_hz: u32,
    state: State,
    info: CardInfo,
    /// A write may still be programming (DO low until it's done).
    busy: bool,
    /// The capacity changed since `poll` last said so.
    changed: bool,
    next_poll: u64,
    pub stats: SdStats,
}

impl<B: SdBus> SdCard<B> {
    /// A card on `bus`, run at `hz` once initialised. Nothing is sent until
    /// `poll` (or `start`) looks for a card.
    pub fn new(bus: B, hz: u32) -> Self {
        SdCard {
            bus,
            hz,
            run_hz: hz.min(DEFAULT_HZ),
            state: State::Absent,
            info: CardInfo { sectors: 0, hc: false, cid: [0; 16] },
            busy: false,
            changed: false,
            next_poll: 0,
            stats: SdStats::default(),
        }
    }

    /// Look for a card now (at boot, so a card that is in is there when the
    /// guest probes the device). True if one initialised.
    pub fn start(&mut self) -> bool {
        if self.init().is_ok() {
            self.state = State::Ready;
        }
        self.next_poll = self.bus.now_us() + POLL_US;
        self.changed = false;
        self.state == State::Ready
    }

    pub fn present(&self) -> bool {
        self.state == State::Ready
    }

    // ------------------------------------------------------------ the bus

    fn deselect(&mut self) {
        self.bus.select(false);
        // eight clocks with chip select off: the card lets go of DO
        self.bus.xchg(0xFF);
    }

    /// Wait (chip select on) until DO is high: the card isn't busy.
    fn ready(&mut self, us: u64) -> Result<(), SdError> {
        if self.bus.xchg(0xFF) == 0xFF {
            self.busy = false;
            return Ok(());
        }
        let t0 = self.bus.now_us();
        loop {
            if self.bus.xchg(0xFF) == 0xFF {
                let waited = self.bus.now_us() - t0;
                self.stats.busy_us_max = self.stats.busy_us_max.max(waited.min(u64::from(u32::MAX)) as u32);
                self.busy = false;
                return Ok(());
            }
            if self.bus.now_us() - t0 > us {
                return Err(SdError::Timeout);
            }
        }
    }

    /// One command (chip select on), after the card is ready; its R1.
    fn command(&mut self, idx: u8, arg: u32) -> Result<u8, SdError> {
        // (CMD12 interrupts a multi-block read: DO is the card's data)
        if idx != 12 {
            self.ready(BUSY_US)?;
        }
        let a = arg.to_be_bytes();
        let mut f = [0x40 | idx, a[0], a[1], a[2], a[3], 0];
        f[5] = crc7(&f[..5]) << 1 | 1;
        self.bus.send(&f);
        if idx == 12 {
            // CMD12: a stuff byte before the response
            self.bus.xchg(0xFF);
        }
        // the response comes within 8 bytes (Ncr)
        for _ in 0..16 {
            let b = self.bus.xchg(0xFF);
            if b & 0x80 == 0 {
                return Ok(b);
            }
        }
        Err(SdError::NoResponse)
    }

    /// An application command (CMD55 first).
    fn acmd(&mut self, idx: u8, arg: u32) -> Result<u8, SdError> {
        let r = self.command(55, 0)?;
        if r & !R1_IDLE != 0 {
            return Ok(r);
        }
        self.command(idx, arg)
    }

    /// Wait for a data block's start token.
    fn token(&mut self) -> Result<(), SdError> {
        let t0 = self.bus.now_us();
        loop {
            match self.bus.xchg(0xFF) {
                TOKEN_START => return Ok(()),
                0xFF => {}
                t => return Err(SdError::Token(t)),
            }
            if self.bus.now_us() - t0 > READ_US {
                return Err(SdError::Timeout);
            }
        }
    }

    /// One data block after its token: `buf`, then its CRC16.
    fn data_in(&mut self, buf: &mut [u8]) -> Result<(), SdError> {
        self.token()?;
        self.bus.recv(buf);
        let mut c = [0xFF; 2];
        self.bus.recv(&mut c);
        if crc16(buf) != u16::from_be_bytes(c) {
            self.crc_error();
            return Err(SdError::Crc);
        }
        Ok(())
    }

    // ------------------------------------------------------------ setup

    fn crc_error(&mut self) {
        self.stats.crc_errors += 1;
        if self.run_hz > DEFAULT_HZ {
            self.stats.fast_crc_errors += 1;
            if self.stats.fast_crc_errors >= FAST_CRC_LIMIT {
                self.run_hz = DEFAULT_HZ;
                self.bus.set_hz(DEFAULT_HZ);
            }
        }
    }

    /// Bring a card from power-up (or any state) to transfer state at `hz`.
    fn init(&mut self) -> Result<(), SdError> {
        self.busy = false;
        self.bus.set_hz(INIT_HZ);
        self.bus.select(false);
        // at least 74 clocks with chip select off
        self.bus.send(&[0xFF; 10]);
        let r = self.init_steps();
        self.deselect();
        let (info, high) = r?;
        self.run_hz = if high { self.hz } else { self.hz.min(DEFAULT_HZ) };
        self.bus.set_hz(self.run_hz);
        self.stats.inits += 1;
        self.info = info;
        Ok(())
    }

    fn init_steps(&mut self) -> Result<(CardInfo, bool), SdError> {
        self.bus.select(true);
        // CMD0 with chip select on: SPI mode, idle state
        let mut r = Err(SdError::NoResponse);
        for _ in 0..4 {
            r = self.command(0, 0);
            if r == Ok(R1_IDLE) {
                break;
            }
        }
        if r? != R1_IDLE {
            return Err(SdError::R1(r?));
        }
        // CMD8: 2.7-3.6 V, check pattern 0xAA; an SD v1 card rejects it
        let r = self.command(8, 0x1AA)?;
        let v2 = r & R1_ILLEGAL == 0;
        if v2 {
            let mut r7 = [0xFF; 4];
            self.bus.recv(&mut r7);
            if r7[2] & 0x0F != 1 || r7[3] != 0xAA {
                return Err(SdError::Init);
            }
        }
        // CRC checking on, both ways
        let r = self.command(59, 1)?;
        if r & !R1_IDLE != 0 {
            return Err(SdError::R1(r));
        }
        // ACMD41 until the card leaves idle state (HCS: we take SDHC)
        let t0 = self.bus.now_us();
        loop {
            let r = self.acmd(41, if v2 { 1 << 30 } else { 0 })?;
            if r == 0 {
                break;
            }
            if r != R1_IDLE {
                return Err(SdError::R1(r));
            }
            if self.bus.now_us() - t0 > INIT_US {
                return Err(SdError::Init);
            }
        }
        // CMD58: CCS says block addressing
        let mut hc = false;
        if v2 {
            let r = self.command(58, 0)?;
            if r != 0 {
                return Err(SdError::R1(r));
            }
            let mut ocr = [0xFF; 4];
            self.bus.recv(&mut ocr);
            hc = ocr[0] & 0x40 != 0;
        }
        if !hc {
            let r = self.command(16, 512)?;
            if r != 0 {
                return Err(SdError::R1(r));
            }
        }
        // CSD (size) and CID (identity), each a 16-byte data block
        let mut csd = [0; 16];
        let r = self.command(9, 0)?;
        if r != 0 {
            return Err(SdError::R1(r));
        }
        self.data_in(&mut csd)?;
        let mut cid = [0; 16];
        let r = self.command(10, 0)?;
        if r != 0 {
            return Err(SdError::R1(r));
        }
        self.data_in(&mut cid)?;
        let sectors = csd_sectors(&csd).ok_or(SdError::Unsupported)?;
        // high-speed mode, if asked for and the card has it (CMD6: switch
        // function group 1 to 1; the 64-byte status says what it did)
        let mut high = false;
        if self.hz > DEFAULT_HZ && self.stats.fast_crc_errors < FAST_CRC_LIMIT {
            let r = self.command(6, 0x80FF_FFF1)?;
            if r == 0 {
                let mut st = [0; 64];
                high = self.data_in(&mut st).is_ok() && st[16] & 0x0F == 1;
            }
        }
        Ok((CardInfo { sectors, hc, cid }, high))
    }

    /// The clock the bus runs at for transfers.
    pub fn clock(&self) -> u32 {
        self.run_hz
    }

    /// The card answers CMD13 with no error bits set.
    fn status_ok(&mut self) -> bool {
        self.bus.select(true);
        let r = self.command(13, 0);
        let r2 = if r.is_ok() { self.bus.xchg(0xFF) } else { 0xFF };
        self.deselect();
        r == Ok(0) && r2 == 0
    }

    /// After a failed try: is the card still there and usable? A card that
    /// doesn't answer is initialised again; a different card (CID) is a
    /// removal (the next `poll` finds the new one).
    fn recover(&mut self) -> bool {
        if self.status_ok() {
            return true;
        }
        let old = self.info;
        if self.init().is_ok() && self.info == old {
            return true;
        }
        self.gone();
        false
    }

    fn gone(&mut self) {
        if self.state == State::Ready {
            self.changed = true;
        }
        self.state = State::Absent;
        self.info.sectors = 0;
        self.busy = false;
    }

    // ------------------------------------------------------------ transfers

    fn addr(&self, lba: u64) -> Option<u32> {
        let a = if self.info.hc { lba } else { lba * 512 };
        u32::try_from(a).ok()
    }

    fn read_blocks(&mut self, lba: u64, segs: &mut [&mut [u8]]) -> Result<(), SdError> {
        let addr = self.addr(lba).ok_or(SdError::Unsupported)?;
        let multi = segs.iter().map(|b| b.len()).sum::<usize>() > 512;
        self.bus.select(true);
        let r = self.read_steps(addr, multi, segs);
        // a multi-block read ends with CMD12, whatever happened (R1b: the
        // card may be busy afterwards, which the next command waits out)
        let stop = if multi { self.command(12, 0).map(|_| ()) } else { Ok(()) };
        self.busy = multi;
        self.deselect();
        r.and(stop)
    }

    fn read_steps(&mut self, addr: u32, multi: bool, segs: &mut [&mut [u8]]) -> Result<(), SdError> {
        let r = self.command(if multi { 18 } else { 17 }, addr)?;
        if r != 0 {
            return Err(SdError::R1(r));
        }
        for buf in segs.iter_mut() {
            for blk in buf.chunks_exact_mut(512) {
                self.data_in(blk)?;
            }
        }
        Ok(())
    }

    fn write_blocks(&mut self, lba: u64, segs: &[&[u8]]) -> Result<(), SdError> {
        let addr = self.addr(lba).ok_or(SdError::Unsupported)?;
        let multi = segs.iter().map(|b| b.len()).sum::<usize>() > 512;
        self.bus.select(true);
        let r = self.write_steps(addr, multi, segs);
        // the card programs the last block after we let go: the next
        // command waits for it
        self.busy = true;
        self.deselect();
        r
    }

    fn write_steps(&mut self, addr: u32, multi: bool, segs: &[&[u8]]) -> Result<(), SdError> {
        let r = self.command(if multi { 25 } else { 24 }, addr)?;
        if r != 0 {
            return Err(SdError::R1(r));
        }
        // a byte's gap before the first token (Nwr)
        self.bus.xchg(0xFF);
        let token = if multi { TOKEN_START_MULTI } else { TOKEN_START };
        let n = segs.iter().map(|b| b.len()).sum::<usize>() / 512;
        for (i, blk) in segs.iter().flat_map(|b| b.chunks_exact(512)).enumerate() {
            self.bus.send(&[token]);
            self.bus.send(blk);
            self.bus.send(&crc16(blk).to_be_bytes());
            let resp = self.bus.xchg(0xFF) & 0x1F;
            if resp == 0x0B {
                self.crc_error();
            }
            if resp != DATA_ACCEPTED {
                if multi {
                    let _ = self.ready(BUSY_US);
                    self.bus.send(&[TOKEN_STOP]);
                    self.bus.xchg(0xFF);
                }
                return Err(SdError::Write(resp));
            }
            // between blocks the card must be done with the last one;
            // after a single block the wait is deferred
            if multi || i + 1 < n {
                self.ready(BUSY_US)?;
            }
        }
        if multi {
            self.bus.send(&[TOKEN_STOP]);
            self.bus.xchg(0xFF);
        }
        Ok(())
    }

    /// Sectors in a request of these segment lengths from `sector`, if each
    /// is whole sectors and they fit on the card.
    fn span(&self, sector: u64, lens: impl Iterator<Item = usize>) -> Option<u64> {
        let mut n = 0u64;
        for l in lens {
            if l % 512 != 0 {
                return None;
            }
            n += (l / 512) as u64;
        }
        (n > 0 && sector + n <= crate::virtio::Disk::sectors(self)).then_some(n)
    }

    /// Run a transfer, retrying after `recover`.
    fn io(&mut self, mut f: impl FnMut(&mut Self) -> Result<(), SdError>) -> bool {
        for _ in 0..TRIES {
            if self.state != State::Ready {
                return false;
            }
            match f(self) {
                Ok(()) => return true,
                Err(e) => {
                    self.stats.errors += 1;
                    self.stats.last_error = Some(e);
                    if !self.recover() {
                        break;
                    }
                }
            }
        }
        self.stats.failed += 1;
        false
    }
}

impl<B: SdBus> crate::virtio::Disk for SdCard<B> {
    fn sectors(&self) -> u64 {
        if self.state == State::Ready { self.info.sectors } else { 0 }
    }
    fn read_only(&self) -> bool {
        false
    }
    fn id(&self) -> [u8; 20] {
        *b"rvemu-sd\0\0\0\0\0\0\0\0\0\0\0\0"
    }
    fn read(&mut self, sector: u64, buf: &mut [u8]) -> bool {
        self.read_segs(sector, &mut [buf])
    }
    fn write(&mut self, sector: u64, buf: &[u8]) -> bool {
        self.write_segs(sector, &[buf])
    }
    fn segments(&self) -> u32 {
        SEGMENTS
    }
    fn read_segs(&mut self, sector: u64, segs: &mut [&mut [u8]]) -> bool {
        let Some(n) = self.span(sector, segs.iter().map(|b| b.len())) else {
            return false;
        };
        let ok = self.io(|s| s.read_blocks(sector, segs));
        if ok {
            self.stats.reads += 1;
            self.stats.sectors_read += n;
        }
        ok
    }
    fn write_segs(&mut self, sector: u64, segs: &[&[u8]]) -> bool {
        let Some(n) = self.span(sector, segs.iter().map(|b| b.len())) else {
            return false;
        };
        let ok = self.io(|s| s.write_blocks(sector, segs));
        if ok {
            self.stats.writes += 1;
            self.stats.sectors_written += n;
        }
        ok
    }
    /// About once a second: initialise a card that went in, drop one that
    /// stopped answering. True when the capacity changed.
    fn poll(&mut self) -> bool {
        let now = self.bus.now_us();
        if now >= self.next_poll {
            self.next_poll = now + POLL_US;
            match self.state {
                State::Absent => {
                    if self.init().is_ok() {
                        self.state = State::Ready;
                        self.changed = true;
                    }
                }
                State::Ready => {
                    if !self.status_ok() {
                        self.recover();
                    }
                }
            }
        }
        core::mem::take(&mut self.changed)
    }
}

/// The card's size in 512-byte sectors, from its CSD.
fn csd_sectors(csd: &[u8; 16]) -> Option<u64> {
    match csd[0] >> 6 {
        0 => {
            let read_bl_len = u32::from(csd[5] & 0x0F);
            let c_size = (u64::from(csd[6] & 0x03) << 10) | (u64::from(csd[7]) << 2) | (u64::from(csd[8]) >> 6);
            let mult = (u32::from(csd[9] & 0x03) << 1) | (u32::from(csd[10]) >> 7);
            if !(9..=11).contains(&read_bl_len) {
                return None;
            }
            Some(((c_size + 1) << (mult + 2)) << read_bl_len >> 9)
        }
        1 => {
            let c_size = (u64::from(csd[7] & 0x3F) << 16) | (u64::from(csd[8]) << 8) | u64::from(csd[9]);
            Some((c_size + 1) * 1024)
        }
        _ => None,
    }
}

/// CRC7 (x^7 + x^3 + 1) of a command's first five bytes.
pub fn crc7(data: &[u8]) -> u8 {
    let mut crc = 0u8;
    for &b in data {
        let mut d = b;
        for _ in 0..8 {
            crc <<= 1;
            if (d ^ crc) & 0x80 != 0 {
                crc ^= 0x09;
            }
            d <<= 1;
        }
    }
    crc & 0x7F
}

const CRC16_TABLE: [u16; 256] = {
    let mut t = [0u16; 256];
    let mut i = 0;
    while i < 256 {
        let mut c = (i as u16) << 8;
        let mut k = 0;
        while k < 8 {
            c = if c & 0x8000 != 0 { (c << 1) ^ 0x1021 } else { c << 1 };
            k += 1;
        }
        t[i] = c;
        i += 1;
    }
    t
};

/// CRC16-CCITT (x^16 + x^12 + x^5 + 1, initial 0) of a data block.
pub fn crc16(data: &[u8]) -> u16 {
    let mut crc = 0u16;
    for &b in data {
        crc = (crc << 8) ^ CRC16_TABLE[usize::from((crc >> 8) as u8 ^ b)];
    }
    crc
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crcs() {
        assert_eq!(crc7(&[0x40, 0, 0, 0, 0]) << 1 | 1, 0x95);
        assert_eq!(crc7(&[0x48, 0, 0, 1, 0xAA]) << 1 | 1, 0x87);
        assert_eq!(crc16(&[0xFF; 512]), 0x7FA1);
    }

    #[test]
    fn csd_sizes() {
        // CSD v2, C_SIZE 60499: 60500 * 512 KiB (the board's 32 GB card)
        let mut csd = [0u8; 16];
        csd[0] = 0x40;
        let c = 60499u32;
        csd[7] = (c >> 16) as u8 & 0x3F;
        csd[8] = (c >> 8) as u8;
        csd[9] = c as u8;
        assert_eq!(csd_sectors(&csd), Some(61_952_000));
        // CSD v1: C_SIZE 4095, C_SIZE_MULT 7, READ_BL_LEN 10 (2 GB)
        let mut csd = [0u8; 16];
        csd[5] = 10;
        csd[6] = 0x03;
        csd[7] = 0xFF;
        csd[8] = 0xC0;
        csd[9] = 0x03;
        csd[10] = 0x80;
        assert_eq!(csd_sectors(&csd), Some((2u64 << 30) / 512));
    }
}
