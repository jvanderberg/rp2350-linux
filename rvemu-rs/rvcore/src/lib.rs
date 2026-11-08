//! RV32IMA + Sv32 guest core, shared by the Mac host and (later) the RP2350.
//!
//! `no_std` so it can build for `riscv32imac-unknown-none-elf`. The
//! `instrument` feature adds host-only statistics that need `alloc`.
#![no_std]

#[cfg(feature = "instrument")]
extern crate alloc;

pub mod cpu;
pub mod dev;
pub mod instrument;
mod mmu;
mod sbi;
pub mod virtio;

pub use cpu::*;
