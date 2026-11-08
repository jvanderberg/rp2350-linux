//! OpenSSH packet construction: payload key || header key || seq(u64 BE).
use chacha20::{
    ChaCha20Legacy,
    cipher::{KeyIvInit, StreamCipher, StreamCipherSeek},
};
use poly1305::{Poly1305, universal_hash::KeyInit};
use subtle::ConstantTimeEq;
use zeroize::Zeroize;

pub fn packet(open: bool, src: &[u8], dst: &mut [u8], params: &[u8]) -> Result<usize, i32> {
    if params.len() != 72 {
        return Err(-3);
    }
    let n = if open {
        src.len().checked_sub(16).ok_or(-3)?
    } else {
        src.len()
    };
    let outlen = n + if open { 0 } else { 16 };
    if n < 4 || dst.len() < outlen {
        return Err(-3);
    }
    let mut body = ChaCha20Legacy::new_from_slices(&params[..32], &params[64..]).map_err(|_| -3)?;
    let mut one_time = [0u8; 32];
    body.apply_keystream(&mut one_time);
    let mac = Poly1305::new_from_slice(&one_time).map_err(|_| -3)?;
    one_time.zeroize();
    // Authenticate the encrypted length AND payload before exposing plaintext.
    if open {
        let tag = mac.clone().compute_unpadded(&src[..n]);
        if !bool::from(tag.as_slice().ct_eq(&src[n..])) {
            return Err(-1);
        }
    }
    dst[..n].copy_from_slice(&src[..n]);
    let mut header =
        ChaCha20Legacy::new_from_slices(&params[32..64], &params[64..]).map_err(|_| -3)?;
    header.apply_keystream(&mut dst[..4]);
    body.seek(64u64);
    body.apply_keystream(&mut dst[4..n]);
    if !open {
        let tag = mac.compute_unpadded(&dst[..n]);
        dst[n..n + 16].copy_from_slice(&tag);
    }
    Ok(outlen)
}
