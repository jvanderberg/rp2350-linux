//! Stateless, protocol-independent key agreement, signature, and KEM operations.
use crate::*;
use ed25519_dalek::{Signature, Signer, SigningKey, VerifyingKey};
use subtle::ConstantTimeEq;
use zeroize::Zeroizing;

fn array<const N: usize>(s: &[u8]) -> Result<[u8; N], i32> {
    s.try_into().map_err(|_| -3)
}

pub fn run(
    fid: u32,
    src: &[u8],
    dst: &mut [u8],
    params: &[u8],
    work: &mut sntrup761::Workspace,
) -> Result<usize, i32> {
    // Check complete operation shapes before any output is written.
    let n = match fid {
        X25519 if src.len() == 32 && params.len() == 32 => 32,
        ED25519_PUBLIC if src.is_empty() && params.len() == 32 => 32,
        ED25519_SIGN if params.len() == 32 => 64,
        ED25519_VERIFY if params.len() == 96 => 0,
        SNTRUP761_KEYPAIR if src.is_empty() && params.len() == 32 => 1158 + 1763,
        SNTRUP761_ENCAP if src.len() == 1158 && params.len() == 32 => 1039 + 32,
        SNTRUP761_DECAP if src.len() == 1039 && params.len() == 1763 => 32,
        _ => return Err(-3),
    };
    if dst.len() < n {
        return Err(-3);
    }
    match fid {
        X25519 => x25519(src, dst, params, n),
        ED25519_PUBLIC | ED25519_SIGN => ed25519_sign_or_public(fid, src, dst, params, n),
        ED25519_VERIFY => ed25519_verify(src, params, n),
        SNTRUP761_KEYPAIR => sntrup_keypair(dst, params, work, n),
        SNTRUP761_ENCAP => sntrup_encap(src, dst, params, work, n),
        SNTRUP761_DECAP => sntrup_decap(src, dst, params, work, n),
        _ => unreachable!(),
    }
}

#[inline(never)]
fn x25519(src: &[u8], dst: &mut [u8], params: &[u8], n: usize) -> Result<usize, i32> {
    let secret = Zeroizing::new(array::<32>(params)?);
    let shared = Zeroizing::new(x25519_dalek::x25519(*secret, array(src)?));
    if bool::from(shared.ct_eq(&[0u8; 32])) {
        return Err(-1);
    }
    dst[..32].copy_from_slice(&*shared);
    Ok(n)
}

#[inline(never)]
fn ed25519_sign_or_public(
    fid: u32,
    src: &[u8],
    dst: &mut [u8],
    params: &[u8],
    n: usize,
) -> Result<usize, i32> {
    let seed = Zeroizing::new(array::<32>(params)?);
    let key = SigningKey::from_bytes(&seed);
    if fid == ED25519_PUBLIC {
        dst[..32].copy_from_slice(key.verifying_key().as_bytes());
    } else {
        dst[..64].copy_from_slice(&key.sign(src).to_bytes());
    }
    Ok(n)
}

#[inline(never)]
fn ed25519_verify(src: &[u8], params: &[u8], n: usize) -> Result<usize, i32> {
    let key = VerifyingKey::from_bytes(&array(&params[..32])?).map_err(|_| -1)?;
    let sig = Signature::from_bytes(&array(&params[32..])?);
    key.verify_strict(src, &sig).map_err(|_| -1)?;
    Ok(n)
}

#[inline(never)]
fn sntrup_keypair(
    dst: &mut [u8],
    params: &[u8],
    work: &mut sntrup761::Workspace,
    n: usize,
) -> Result<usize, i32> {
    let seed = Zeroizing::new(array::<32>(params)?);
    let (pk, sk) = sntrup761::generate_key_from_seed_with_workspace(*seed, work);
    dst[..1158].copy_from_slice(pk.as_ref());
    dst[1158..n].copy_from_slice(sk.as_ref());
    Ok(n)
}

#[inline(never)]
fn sntrup_encap(
    src: &[u8],
    dst: &mut [u8],
    params: &[u8],
    work: &mut sntrup761::Workspace,
    n: usize,
) -> Result<usize, i32> {
    let seed = Zeroizing::new(array::<32>(params)?);
    let pk = sntrup761::EncapsulationKey::try_from(src).map_err(|_| -3)?;
    let (ct, ss) = pk.encapsulate_deterministic_with_workspace(*seed, work);
    dst[..1039].copy_from_slice(ct.as_ref());
    dst[1039..n].copy_from_slice(ss.as_ref());
    Ok(n)
}

#[inline(never)]
fn sntrup_decap(
    src: &[u8],
    dst: &mut [u8],
    params: &[u8],
    work: &mut sntrup761::Workspace,
    n: usize,
) -> Result<usize, i32> {
    let sk = sntrup761::DecapsulationKey::try_from(params).map_err(|_| -3)?;
    let ct = sntrup761::Ciphertext::try_from(src).map_err(|_| -3)?;
    // KEM implicit rejection deliberately returns a fallback secret for
    // invalid ciphertext; do not turn it into a validity oracle.
    dst[..32].copy_from_slice(sk.decapsulate_with_workspace(&ct, work).as_ref());
    Ok(n)
}
