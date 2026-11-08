//! Allocation-free native buffer engine: compression, AEAD, key agreement,
//! signatures and KEMs. Also callable by synthetic guests for equivalence
//! checks. Protocol policy, key storage and randomness ownership are external.
#![no_std]
use chacha20poly1305::{ChaCha20Poly1305, KeyInit, Nonce, Tag, aead::AeadInPlace};

pub const MAX_BUFFER: usize = 32768;
pub const MAX_PARAMS: usize = 4096;
pub const LZ4_DECODE: u32 = 0;
pub const LZ4_ENCODE: u32 = 1;
pub const AEAD_SEAL: u32 = 2;
pub const AEAD_OPEN: u32 = 3;
pub const SSH_SEAL: u32 = 4;
pub const SSH_OPEN: u32 = 5;
pub const X25519: u32 = 6;
pub const ED25519_PUBLIC: u32 = 7;
pub const ED25519_SIGN: u32 = 8;
pub const ED25519_VERIFY: u32 = 9;
pub const SNTRUP761_KEYPAIR: u32 = 10;
pub const SNTRUP761_ENCAP: u32 = 11;
pub const SNTRUP761_DECAP: u32 = 12;
pub const OP_COUNT: usize = 13;
mod asymmetric;
mod ssh;
mod workspace;
pub use workspace::{STACK_BYTES, Workspace};

/// Raw LZ4 blocks, or IETF ChaCha20-Poly1305 ciphertext with appended tag.
/// IETF AEAD parameters are key[32] || nonce[12] || AAD[0..1024].
/// SSH parameters are payload_key[32] || header_key[32] || sequence[8 BE]. Caller owns
/// nonce uniqueness. On error output may have changed; never consume it.
/// IETF AEAD authentication failure clears its copied ciphertext buffer.
/// Primitive contracts for FIDs 6–12 are documented in guest/README.md.
/// Convenience entry point with stack-local scratch. Firmware must use
/// `Workspace::run` with explicitly reserved storage instead.
#[inline(never)]
pub fn run(fid: u32, src: &[u8], dst: &mut [u8], params: &[u8]) -> Result<usize, i32> {
    run_inner(fid, src, dst, params, &mut sntrup761::Workspace::new())
}

#[inline(never)]
fn run_inner(
    fid: u32,
    src: &[u8],
    dst: &mut [u8],
    params: &[u8],
    work: &mut sntrup761::Workspace,
) -> Result<usize, i32> {
    if fid >= OP_COUNT as u32 {
        return Err(-2);
    }
    if src.len() > MAX_BUFFER || dst.len() > MAX_BUFFER || params.len() > MAX_PARAMS {
        return Err(-3);
    }
    match fid {
        X25519..=SNTRUP761_DECAP => asymmetric::run(fid, src, dst, params, work),
        SSH_SEAL | SSH_OPEN => ssh::packet(fid == SSH_OPEN, src, dst, params),
        LZ4_DECODE | LZ4_ENCODE => {
            if !params.is_empty() {
                return Err(-3);
            }
            if fid == LZ4_DECODE {
                lz4_flex::block::decompress_into(src, dst).map_err(|_| -1)
            } else {
                compress(src, dst)
            }
        }
        _ => {
            if params.len() < 44 || params.len() > 1068 {
                return Err(-3);
            }
            let cipher = ChaCha20Poly1305::new_from_slice(&params[..32]).map_err(|_| -3)?;
            let nonce = Nonce::from_slice(&params[32..44]);
            let aad = &params[44..];
            if fid == AEAD_SEAL {
                let n = src.len();
                if n + 16 > dst.len() {
                    return Err(-3);
                }
                dst[..n].copy_from_slice(src);
                let tag = cipher
                    .encrypt_in_place_detached(nonce, aad, &mut dst[..n])
                    .map_err(|_| -1)?;
                dst[n..n + 16].copy_from_slice(&tag);
                Ok(n + 16)
            } else {
                let n = src.len().checked_sub(16).ok_or(-3)?;
                if n > dst.len() {
                    return Err(-3);
                }
                dst[..n].copy_from_slice(&src[..n]);
                if cipher
                    .decrypt_in_place_detached(
                        nonce,
                        aad,
                        &mut dst[..n],
                        Tag::from_slice(&src[n..]),
                    )
                    .is_err()
                {
                    dst[..n].fill(0);
                    return Err(-1);
                }
                Ok(n)
            }
        }
    }
}

#[inline(never)]
fn compress(src: &[u8], dst: &mut [u8]) -> Result<usize, i32> {
    lz4_flex::block::compress_into(src, dst).map_err(|_| -1)
}
