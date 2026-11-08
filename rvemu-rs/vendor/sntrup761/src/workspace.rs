//! Reusable scalar scratch storage, added by rvemu. Arithmetic is unchanged.
use crate::P;
use zeroize::{Zeroize, ZeroizeOnDrop};
#[derive(Zeroize)]
pub(crate) struct RqReciprocal {
    pub f: [i16; P + 1],
    pub g: [i16; P + 1],
    pub u: [i16; 2 * P + 2],
    pub v: [i16; 2 * P + 2],
}
#[derive(Zeroize)]
pub(crate) struct R3Reciprocal {
    pub f: [i8; P + 1],
    pub g: [i8; P + 1],
    pub u: [i8; 2 * P + 2],
    pub v: [i8; 2 * P + 2],
}
#[derive(Zeroize)]
pub(crate) struct Encoding {
    pub r: [u16; P],
    pub m: [u16; P],
    pub all_m: [u16; 1525],
    pub input: [u8; 1200],
}
/// Borrow exclusively for one KEM operation; may live in SRAM or external RAM.
/// Public workspace APIs wipe scratch before returning, including implicit rejection.
#[derive(Zeroize, ZeroizeOnDrop)]
pub struct Workspace {
    pub(crate) rq: RqReciprocal,
    pub(crate) r3: R3Reciprocal,
    pub(crate) encoding: Encoding,
    pub(crate) rq_product: [i16; 2 * P - 1],
    pub(crate) r3_product: [i8; 2 * P - 1],
    pub(crate) random: [i32; P],
}
impl Workspace {
    pub const fn new() -> Self {
        Self {
            rq: RqReciprocal {
                f: [0; P + 1],
                g: [0; P + 1],
                u: [0; 2 * P + 2],
                v: [0; 2 * P + 2],
            },
            r3: R3Reciprocal {
                f: [0; P + 1],
                g: [0; P + 1],
                u: [0; 2 * P + 2],
                v: [0; 2 * P + 2],
            },
            encoding: Encoding {
                r: [0; P],
                m: [0; P],
                all_m: [0; 1525],
                input: [0; 1200],
            },
            rq_product: [0; 2 * P - 1],
            r3_product: [0; 2 * P - 1],
            random: [0; P],
        }
    }
}
impl Default for Workspace {
    fn default() -> Self {
        Self::new()
    }
}
