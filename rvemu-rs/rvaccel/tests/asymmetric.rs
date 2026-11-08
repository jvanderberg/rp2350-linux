use rvaccel::*;
fn hex(s: &str) -> Vec<u8> {
    let s = s.trim();
    s.as_bytes()
        .chunks_exact(2)
        .map(|p| u8::from_str_radix(std::str::from_utf8(p).unwrap(), 16).unwrap())
        .collect()
}
#[test]
fn rfc7748_x25519_and_low_order_rejection() {
    let secret = hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    let public = hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    let peer = hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    let shared = hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    let mut out = [0u8; 32];
    let mut base = [0; 32];
    base[0] = 9;
    assert_eq!(run(X25519, &base, &mut out, &secret), Ok(32));
    assert_eq!(out.as_slice(), public);
    assert_eq!(run(X25519, &peer, &mut out, &secret), Ok(32));
    assert_eq!(out.as_slice(), shared);
    for point in [[0; 32], {
        let mut p = [0; 32];
        p[0] = 1;
        p
    }] {
        out.fill(0xa5);
        assert_eq!(run(X25519, &point, &mut out, &secret), Err(-1));
        assert_eq!(out, [0xa5; 32]);
    }
}
#[test]
fn rfc8032_sign_verify_and_general_messages() {
    let seed = hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    let public = hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    let signature = hex(concat!(
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555f",
        "b8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"
    ));
    let mut pk = [0; 32];
    let mut sig = [0; 64];
    assert_eq!(run(ED25519_PUBLIC, &[], &mut pk, &seed), Ok(32));
    assert_eq!(pk.as_slice(), public);
    assert_eq!(run(ED25519_SIGN, &[], &mut sig, &seed), Ok(64));
    assert_eq!(sig.as_slice(), signature);
    let mut params = public.clone();
    params.extend_from_slice(&sig);
    assert_eq!(run(ED25519_VERIFY, &[], &mut [], &params), Ok(0));
    for i in 0..params.len() {
        let mut bad = params.clone();
        bad[i] ^= 1;
        assert_eq!(run(ED25519_VERIFY, &[], &mut [], &bad), Err(-1));
    }
    for msg in [
        &b"firmware manifest: version 42"[..],
        &vec![0x5a; 32768][..],
    ] {
        run(ED25519_SIGN, msg, &mut sig, &seed).unwrap();
        params[32..].copy_from_slice(&sig);
        assert_eq!(run(ED25519_VERIFY, msg, &mut [], &params), Ok(0));
        assert_eq!(
            run(ED25519_VERIFY, b"different message", &mut [], &params),
            Err(-1)
        );
    }
    // Weak public key and noncanonical scalar must never verify.
    params[..32].fill(0);
    assert_eq!(run(ED25519_VERIFY, &[], &mut [], &params), Err(-1));
}
#[test]
fn sntrup_ietf_decapsulation_vectors_and_implicit_rejection() {
    // draft-josefsson-ntruprime-streamlined-00 vectors, also shipped by upstream.
    for (sk, ct, ss) in [
        (
            include_str!("data/sntrup761-kat0-sk.hex"),
            include_str!("data/sntrup761-kat0-ct.hex"),
            include_str!("data/sntrup761-kat0-ss.hex"),
        ),
        (
            include_str!("data/sntrup761-kat1-sk.hex"),
            include_str!("data/sntrup761-kat1-ct.hex"),
            include_str!("data/sntrup761-kat1-ss.hex"),
        ),
    ] {
        let sk = hex(sk);
        let mut ct = hex(ct);
        let ss = hex(ss);
        let mut out = [0; 32];
        assert_eq!(run(SNTRUP761_DECAP, &ct, &mut out, &sk), Ok(32));
        assert_eq!(out.as_slice(), ss);
        ct[0] ^= 1;
        assert_eq!(run(SNTRUP761_DECAP, &ct, &mut out, &sk), Ok(32));
        assert_ne!(out.as_slice(), ss);
        let first = out;
        run(SNTRUP761_DECAP, &ct, &mut out, &sk).unwrap();
        assert_eq!(first, out);
    }
    let mut kp = [0; 2921];
    run(SNTRUP761_KEYPAIR, &[], &mut kp, &[17; 32]).unwrap();
    let mut enc = [0; 1071];
    run(SNTRUP761_ENCAP, &kp[..1158], &mut enc, &[23; 32]).unwrap();
    let mut dec = [0; 32];
    run(SNTRUP761_DECAP, &enc[..1039], &mut dec, &kp[1158..]).unwrap();
    assert_eq!(dec, enc[1039..]);
}
#[test]
fn asymmetric_shapes_fail_without_writing() {
    for (op, src, params, n) in [
        (X25519, 32, 32, 32),
        (ED25519_PUBLIC, 0, 32, 32),
        (ED25519_SIGN, 10, 32, 64),
        (ED25519_VERIFY, 10, 96, 0),
        (SNTRUP761_KEYPAIR, 0, 32, 2921),
        (SNTRUP761_ENCAP, 1158, 32, 1071),
        (SNTRUP761_DECAP, 1039, 1763, 32),
    ] {
        let input = vec![0; src];
        let mut output = vec![0xa5; n];
        assert_eq!(run(op, &input, &mut output, &vec![0; params - 1]), Err(-3));
        assert!(output.iter().all(|&x| x == 0xa5));
        if n > 0 {
            assert_eq!(
                run(op, &input, &mut output[..n - 1], &vec![0; params]),
                Err(-3)
            );
            assert!(output.iter().all(|&x| x == 0xa5));
        }
    }
}
