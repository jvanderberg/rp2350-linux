//! RV32I instruction encoder writing into a word buffer.

pub type Reg = u32;
pub const ZERO: Reg = 0;
pub const RA: Reg = 1;
pub const SP: Reg = 2;

#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
pub const fn r_type(op: u32, rd: Reg, f3: u32, rs1: Reg, rs2: Reg, f7: u32) -> u32 {
    (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op
}

#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
pub const fn i_type(op: u32, rd: Reg, f3: u32, rs1: Reg, imm: i32) -> u32 {
    (((imm as u32) & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op
}

#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
pub const fn s_type(op: u32, f3: u32, rs1: Reg, rs2: Reg, imm: i32) -> u32 {
    let imm = imm as u32;
    (((imm >> 5) & 0x7F) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((imm & 0x1F) << 7) | op
}

#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
pub const fn b_type(f3: u32, rs1: Reg, rs2: Reg, off: i32) -> u32 {
    let o = off as u32;
    (((o >> 12) & 1) << 31)
        | (((o >> 5) & 0x3F) << 25)
        | (rs2 << 20)
        | (rs1 << 15)
        | (f3 << 12)
        | (((o >> 1) & 0xF) << 8)
        | (((o >> 11) & 1) << 7)
        | 0x63
}

#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
pub const fn j_type(rd: Reg, off: i32) -> u32 {
    let o = off as u32;
    (((o >> 20) & 1) << 31)
        | (((o >> 1) & 0x3FF) << 21)
        | (((o >> 11) & 1) << 20)
        | (((o >> 12) & 0xFF) << 12)
        | (rd << 7)
        | 0x6F
}

#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
pub const fn fits_i12(v: i32) -> bool {
    v >= -2048 && v <= 2047
}

/// Word-addressed code emitter. `base` is the host address of `buf[0]`.
pub struct Asm<'a> {
    buf: &'a mut [u32],
    base: usize,
    pos: usize,
    /// `vpos() - pos()`
    shift: isize,
    overflow: bool,
}

impl<'a> Asm<'a> {
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn new(buf: &'a mut [u32], base: usize) -> Self {
        Asm { buf, base, pos: 0, shift: 0, overflow: false }
    }

    /// Continue emitting at word `pos` of `buf`.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn at(buf: &'a mut [u32], base: usize, pos: usize) -> Self {
        Asm { buf, base, pos, shift: 0, overflow: false }
    }

    /// Emit from `buf[0]` as if at word `virt` of the code buffer: jumps
    /// into the shared routines (`vpos()`-relative) then hold once the words
    /// are copied to `virt`, or relinked from there.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn at_virtual(buf: &'a mut [u32], virt: usize) -> Self {
        Asm { buf, base: 0, pos: 0, shift: virt as isize, overflow: false }
    }

    /// Word index in the code buffer the next word is emitted for.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn vpos(&self) -> isize {
        self.pos as isize + self.shift
    }

    /// Number of words emitted.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn pos(&self) -> usize {
        self.pos
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn overflowed(&self) -> bool {
        self.overflow
    }
    /// Host address of word `pos`.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn addr(&self, pos: usize) -> usize {
        self.base + pos * 4
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn emit(&mut self, w: u32) -> usize {
        let p = self.pos;
        if p < self.buf.len() {
            self.buf[p] = w;
        } else {
            self.overflow = true;
        }
        self.pos += 1;
        p
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn patch(&mut self, pos: usize, w: u32) {
        if pos < self.buf.len() {
            self.buf[pos] = w;
        }
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn word(&self, pos: usize) -> u32 {
        self.buf.get(pos).copied().unwrap_or(0)
    }

    // ---- common instructions
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn addi(&mut self, rd: Reg, rs1: Reg, imm: i32) {
        debug_assert!(fits_i12(imm));
        self.emit(i_type(0x13, rd, 0, rs1, imm));
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn andi(&mut self, rd: Reg, rs1: Reg, imm: i32) {
        self.emit(i_type(0x13, rd, 7, rs1, imm));
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn slli(&mut self, rd: Reg, rs1: Reg, sh: u32) {
        self.emit(i_type(0x13, rd, 1, rs1, sh as i32));
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn srli(&mut self, rd: Reg, rs1: Reg, sh: u32) {
        self.emit(i_type(0x13, rd, 5, rs1, sh as i32));
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn srai(&mut self, rd: Reg, rs1: Reg, sh: u32) {
        self.emit(i_type(0x13, rd, 5, rs1, 0x400 | sh as i32));
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn add(&mut self, rd: Reg, rs1: Reg, rs2: Reg) {
        self.emit(r_type(0x33, rd, 0, rs1, rs2, 0));
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn sub(&mut self, rd: Reg, rs1: Reg, rs2: Reg) {
        self.emit(r_type(0x33, rd, 0, rs1, rs2, 0x20));
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn xor(&mut self, rd: Reg, rs1: Reg, rs2: Reg) {
        self.emit(r_type(0x33, rd, 4, rs1, rs2, 0));
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn or(&mut self, rd: Reg, rs1: Reg, rs2: Reg) {
        self.emit(r_type(0x33, rd, 6, rs1, rs2, 0));
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn and(&mut self, rd: Reg, rs1: Reg, rs2: Reg) {
        self.emit(r_type(0x33, rd, 7, rs1, rs2, 0));
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn ori(&mut self, rd: Reg, rs1: Reg, imm: i32) {
        self.emit(i_type(0x13, rd, 6, rs1, imm));
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn xori(&mut self, rd: Reg, rs1: Reg, imm: i32) {
        self.emit(i_type(0x13, rd, 4, rs1, imm));
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn mul(&mut self, rd: Reg, rs1: Reg, rs2: Reg) {
        self.emit(r_type(0x33, rd, 0, rs1, rs2, 1));
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn lui(&mut self, rd: Reg, imm: u32) {
        self.emit((imm & 0xFFFF_F000) | (rd << 7) | 0x37);
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn lw(&mut self, rd: Reg, rs1: Reg, off: i32) {
        self.emit(i_type(0x03, rd, 2, rs1, off));
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn sw(&mut self, rs2: Reg, rs1: Reg, off: i32) {
        self.emit(s_type(0x23, 2, rs1, rs2, off));
    }
    /// Load with guest funct3 (lb/lh/lw/lbu/lhu).
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn load(&mut self, f3: u32, rd: Reg, rs1: Reg, off: i32) {
        self.emit(i_type(0x03, rd, f3, rs1, off));
    }
    /// Store with guest funct3 (sb/sh/sw).
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn store(&mut self, f3: u32, rs2: Reg, rs1: Reg, off: i32) {
        self.emit(s_type(0x23, f3, rs1, rs2, off));
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn jalr(&mut self, rd: Reg, rs1: Reg, off: i32) {
        self.emit(i_type(0x67, rd, 0, rs1, off));
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn ret(&mut self) {
        self.jalr(ZERO, RA, 0);
    }

    /// Load a 32-bit constant.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn li(&mut self, rd: Reg, v: u32) {
        let lo = ((v as i32) << 20) >> 20; // sign-extended low 12 bits
        let hi = v.wrapping_sub(lo as u32);
        if hi == 0 {
            self.addi(rd, ZERO, lo);
        } else {
            self.lui(rd, hi);
            if lo != 0 {
                self.addi(rd, rd, lo);
            }
        }
    }

    /// Branch/jump placeholders, fixed up once the target is known.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn branch_fwd(&mut self, f3: u32, rs1: Reg, rs2: Reg) -> usize {
        self.emit(b_type(f3, rs1, rs2, 0))
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn jump_fwd(&mut self) -> usize {
        self.emit(j_type(ZERO, 0))
    }

    /// Point the branch or jal at `at` to word `target`. Returns false if
    /// out of range.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn fix(&mut self, at: usize, target: usize) -> bool {
        let off = (target as isize - at as isize) * 4;
        let w = self.word(at);
        let off = off as i32;
        if w & 0x7F == 0x63 {
            if !(-4096..4096).contains(&off) {
                return false;
            }
            let f3 = (w >> 12) & 7;
            let rs1 = (w >> 15) & 31;
            let rs2 = (w >> 20) & 31;
            self.patch(at, b_type(f3, rs1, rs2, off));
        } else {
            if !(-(1 << 20)..(1 << 20)).contains(&off) {
                return false;
            }
            let rd = (w >> 7) & 31;
            self.patch(at, j_type(rd, off));
        }
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Decoders matching the interpreter's.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn imm_b(insn: u32) -> i32 {
        (((insn as i32) >> 31) << 12)
            | ((insn << 4) & 0x800) as i32
            | ((insn >> 20) & 0x7E0) as i32
            | ((insn >> 7) & 0x1E) as i32
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn imm_j(insn: u32) -> i32 {
        (((insn as i32) >> 31) << 20)
            | (insn & 0xF_F000) as i32
            | ((insn >> 9) & 0x800) as i32
            | ((insn >> 20) & 0x7FE) as i32
    }
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn imm_s(insn: u32) -> i32 {
        (((insn as i32) >> 25) << 5) | ((insn >> 7) & 0x1F) as i32
    }

    #[test]
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn known_encodings() {
        assert_eq!(i_type(0x13, 10, 0, 10, 1), 0x0015_0513); // addi a0,a0,1
        assert_eq!(i_type(0x03, 11, 2, 2, 4), 0x0041_2583); // lw a1,4(sp)
        assert_eq!(s_type(0x23, 2, 2, 11, 8), 0x00b1_2423); // sw a1,8(sp)
        assert_eq!(b_type(0, 10, 11, 16), 0x00b5_0863); // beq a0,a1,16
        assert_eq!(j_type(1, 2048), 0x0010_00ef); // jal ra,2048
        assert_eq!(r_type(0x33, 10, 0, 11, 12, 0x20), 0x40c5_8533); // sub a0,a1,a2
    }

    #[test]
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn immediates_roundtrip() {
        for off in (-4096..4096).step_by(2) {
            assert_eq!(imm_b(b_type(0, 1, 2, off)), off);
        }
        for off in (-(1 << 20)..(1 << 20)).step_by(2 * 997) {
            let off = off & !1;
            assert_eq!(imm_j(j_type(0, off)), off);
        }
        for imm in -2048..2048 {
            assert_eq!(imm_s(s_type(0x23, 2, 1, 2, imm)), imm);
        }
    }

    #[test]
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn li_values() {
        for &v in &[0u32, 1, 0x7FF, 0x800, 0xFFF, 0x1000, 0x8000_0000, 0xFFFF_FFFF, 0x1234_5FFF, 0xC000_0800] {
            let mut buf = [0u32; 2];
            let mut a = Asm::new(&mut buf, 0);
            a.li(5, v);
            let n = a.pos();
            // evaluate
            let mut r = 0u32;
            for &w in &buf[..n] {
                match w & 0x7F {
                    0x37 => r = w & 0xFFFF_F000,
                    0x13 => {
                        let rs1 = (w >> 15) & 31;
                        let base = if rs1 == 0 { 0 } else { r };
                        r = base.wrapping_add(((w as i32) >> 20) as u32);
                    }
                    _ => panic!(),
                }
            }
            assert_eq!(r, v, "li {v:#x}");
        }
    }
}
