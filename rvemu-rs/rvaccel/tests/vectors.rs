use rvaccel::*;
fn hex(s: &str) -> Vec<u8> {
    s.as_bytes()
        .chunks_exact(2)
        .map(|p| u8::from_str_radix(std::str::from_utf8(p).unwrap(), 16).unwrap())
        .collect()
}
#[test]
fn rfc8439_and_tampering() {
    // RFC 8439 section 2.8.2, independent expected ciphertext and tag.
    let plain = b"Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    let mut params: Vec<u8> = (0x80..=0x9f).collect();
    params.extend(hex("07000000404142434445464750515253c0c1c2c3c4c5c6c7"));
    let expected = hex(concat!(
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6",
        "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36",
        "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc",
        "3ff4def08e4b7a9de576d26586cec64b61161ae10b594f09e26a7e902ecbd0600691"
    ));
    let mut out = [0u8; 256];
    assert_eq!(run(AEAD_SEAL, plain, &mut out, &params), Ok(expected.len()));
    assert_eq!(&out[..expected.len()], expected);
    assert_eq!(
        run(AEAD_OPEN, &expected, &mut out, &params),
        Ok(plain.len())
    );
    assert_eq!(&out[..plain.len()], plain);
    for i in 0..expected.len() {
        let mut bad = expected.clone();
        bad[i] ^= 1;
        out.fill(0xa5);
        assert_eq!(run(AEAD_OPEN, &bad, &mut out, &params), Err(-1));
        assert!(out[..plain.len()].iter().all(|&b| b == 0));
    }
    for i in 0..params.len() {
        let mut bad = params.clone();
        bad[i] ^= 1;
        assert_eq!(run(AEAD_OPEN, &expected, &mut out, &bad), Err(-1));
    }
    assert_eq!(run(AEAD_OPEN, &expected[..15], &mut out, &params), Err(-3));
    assert_eq!(run(AEAD_SEAL, plain, &mut out[..10], &params), Err(-3));
    let n = run(AEAD_SEAL, &[], &mut out, &params).unwrap();
    assert_eq!(n, 16);
    let tag = out[..n].to_vec();
    assert_eq!(run(AEAD_OPEN, &tag, &mut [], &params), Ok(0));
}
#[test]
fn lz4_bounds_malformed_and_roundtrip() {
    let mut out = [0xa5; 32768];
    assert_eq!(run(LZ4_DECODE, b"\x50hello", &mut out, &[]), Ok(5));
    assert_eq!(&out[..5], b"hello");
    for bad in [&b"\xf0"[..], &b"\x10a\0\0"[..], &b"\x00\x01\x00"[..]] {
        assert!(run(LZ4_DECODE, bad, &mut out, &[]).is_err());
    }
    assert!(run(LZ4_DECODE, b"\x50hello", &mut out[..4], &[]).is_err());
    let mut seed = 0x12345678u32;
    for n in [0, 1, 15, 16, 63, 64, 4096, 16384] {
        for random in [false, true] {
            let src: Vec<u8> = (0..n)
                .map(|i| {
                    seed ^= seed << 13;
                    seed ^= seed >> 17;
                    seed ^= seed << 5;
                    if random { seed as u8 } else { (i % 31) as u8 }
                })
                .collect();
            let len = run(LZ4_ENCODE, &src, &mut out, &[]).unwrap();
            let mut decoded = vec![0; n];
            assert_eq!(run(LZ4_DECODE, &out[..len], &mut decoded, &[]), Ok(n));
            assert_eq!(src, decoded);
        }
    }
    assert_eq!(run(99, &[], &mut out, &[]), Err(-2));
    assert_eq!(run(LZ4_DECODE, &[], &mut out, &[0]), Err(-3));
    assert_eq!(
        run(LZ4_DECODE, &vec![0; MAX_BUFFER + 1], &mut out, &[]),
        Err(-3)
    );
}

#[test]
fn ssh_independent_packet_vector() {
    // draft-ietf-sshm-chacha20-poly1305-04 Appendix A, full wire packet.
    let plain = hex(concat!(
        "00000048065e00000000000000384c6f",
        "72656d20697073756d20646f6c6f7220",
        "73697420616d65742c20636f6e736563",
        "7465747572206164697069736963696e",
        "6720656c69744e43e804dc6c"
    ));
    let params = hex(concat!(
        "8bbff6855fc102338c373e73aac0c914",
        "f076a905b2444a32eecaffeae22becc5",
        "e9b7a7a5825a8249346ec1c28301cf39",
        "4543fc7569887d76e168f37562ac0740",
        "0000000000000007"
    ));
    let wire = hex(concat!(
        "2c3ecce4a5bc05895bf07a7ba956b6c6",
        "8829ac7c83b780b7000ecde745afc705",
        "bbc378ce03a280236b87b53bed583966",
        "2302b164b6286a48cd1e097138e3cb90",
        "9b8b2b829dd18d2a35ff82d995349e85",
        "5bf02c298ef775f2d1a7e8b8"
    ));
    let mut out = vec![0xa5; wire.len()];
    assert_eq!(run(SSH_SEAL, &plain, &mut out, &params), Ok(wire.len()));
    assert_eq!(out, wire);
    assert_eq!(run(SSH_OPEN, &wire, &mut out, &params), Ok(plain.len()));
    assert_eq!(&out[..plain.len()], plain);
    for i in 0..wire.len() {
        let mut bad = wire.clone();
        bad[i] ^= 1;
        out.fill(0xa5);
        assert_eq!(run(SSH_OPEN, &bad, &mut out, &params), Err(-1));
        assert!(out.iter().all(|&v| v == 0xa5)); // auth failure exposes no plaintext
    }
    for i in [0, 64, 71] {
        let mut bad = params.clone();
        bad[i] ^= 1;
        assert_eq!(run(SSH_OPEN, &wire, &mut out, &bad), Err(-1));
    }
    // SSH authenticates wire bytes with the body key; changing only the
    // header key changes the decoded length, not the MAC verification.
    let mut wrong_header = params.clone();
    wrong_header[32] ^= 1;
    assert_eq!(
        run(SSH_OPEN, &wire, &mut out, &wrong_header),
        Ok(plain.len())
    );
    assert_ne!(&out[..4], &plain[..4]);
    assert_eq!(run(SSH_SEAL, &plain[..3], &mut out, &params), Err(-3));
    assert_eq!(run(SSH_OPEN, &wire, &mut out[..1], &params), Err(-3));
    assert_eq!(run(SSH_OPEN, &wire, &mut out, &params[..71]), Err(-3));
}
