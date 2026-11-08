//! RV32IMA + Sv32 guest core, shared by the Mac host and (later) the RP2350.
//!
//! `no_std` so it can build for `riscv32imac-unknown-none-elf`. The
//! `instrument` feature adds host-only statistics that need `alloc`.
#![no_std]

#[cfg(feature = "instrument")]
extern crate alloc;

pub mod cpu;
pub mod benchmark;
pub mod dev;
pub mod instrument;
mod mmu;
pub mod nor;
pub mod periph;
mod sbi;
#[cfg(feature = "pv-pages")]
mod pv_pages;
#[cfg(feature = "pv-buffers")]
mod pv_buffers;
/// Native libraries are not represented in the instruction/cache timing model.
pub const NATIVE_BUFFERS: bool = cfg!(feature = "pv-buffers");
pub mod swapdev;
pub mod virtio;

pub use cpu::*;

/// The `rng-seed` the generated device trees carry in `/chosen`, so the guest
/// kernel's CRNG is ready at boot instead of never (it has no other entropy
/// source, and without it every `get_random_*` re-extracts its pool). These
/// fixed bytes keep batch runs and the benchmark deterministic; whatever
/// boots a guest for interactive use (the shell firmware, the Mac host
/// without `-B`) overwrites them in the DTB with real randomness.
pub const DTB_RNG_SEED: [u8; 32] = *b"rvemu: fixed DTB rng-seed bytes!";

#[cfg(feature = "pv-buffers")]
pub use rvaccel::Workspace as NativeWorkspace;
#[cfg(feature = "pv-buffers")]
pub use rvaccel::STACK_BYTES as NATIVE_STACK_BYTES;
/// The native buffer SBI's pause protocol (FID flag, continue, abort, the
/// pending error).
#[cfg(feature = "pv-buffers")]
pub use pv_buffers::{ABORT as NATIVE_ABORT, CONTINUE as NATIVE_CONTINUE, ERR_PENDING as NATIVE_PENDING, MAY_PEND as NATIVE_MAY_PEND};
