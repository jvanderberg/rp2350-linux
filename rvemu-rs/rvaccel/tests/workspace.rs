use rvaccel::*;
use sntrup761::sha2::{Digest, Sha512};

#[test]
fn reused_workspace_matches_unmodified_upstream() {
    let mut workspace = Workspace::new();
    let mut keys = [0u8; 2921];
    let mut enc = [0u8; 1071];
    let mut secret = [0u8; 32];
    for (i, expected) in include_str!("data/sntrup761-upstream.sha512")
        .lines()
        .enumerate()
    {
        assert_eq!(
            workspace.run(SNTRUP761_KEYPAIR, &[], &mut keys, &[i as u8; 32]),
            Ok(2921)
        );
        assert_eq!(
            workspace.run(
                SNTRUP761_ENCAP,
                &keys[..1158],
                &mut enc,
                &[i as u8 + 64; 32]
            ),
            Ok(1071)
        );
        assert_eq!(
            workspace.run(SNTRUP761_DECAP, &enc[..1039], &mut secret, &keys[1158..]),
            Ok(32)
        );
        assert_eq!(secret, enc[1039..]);
        let mut hash = Sha512::new();
        hash.update(keys);
        hash.update(enc);
        enc[i * 61] ^= 1;
        assert_eq!(
            workspace.run(SNTRUP761_DECAP, &enc[..1039], &mut secret, &keys[1158..]),
            Ok(32)
        );
        assert_ne!(secret, enc[1039..]);
        hash.update(secret);
        let actual: String = hash.finalize().iter().map(|b| format!("{b:02x}")).collect();
        assert_eq!(actual, expected, "upstream case {i}");
        // Failures and unrelated operations between KEM calls must not poison reuse.
        assert_eq!(
            workspace.run(SNTRUP761_KEYPAIR, &[], &mut secret, &[0; 32]),
            Err(-3)
        );
        assert_eq!(workspace.run(99, &[], &mut [], &[]), Err(-2));
        assert_eq!(
            workspace.run(LZ4_DECODE, b"\x50hello", &mut secret, &[]),
            Ok(5)
        );
        assert_eq!(&secret[..5], b"hello");
    }
}

#[test]
fn initializes_only_reserved_memory_in_place() {
    #[repr(C, align(16))]
    struct Guarded {
        before: [u8; 16],
        workspace: core::mem::MaybeUninit<Workspace>,
        after: [u8; 16],
    }
    let mut storage = Box::new(Guarded {
        before: [0xa5; 16],
        workspace: core::mem::MaybeUninit::uninit(),
        after: [0x5a; 16],
    });
    let workspace = unsafe { Workspace::init_at(storage.workspace.as_mut_ptr()) };
    let mut out = [0; 32];
    assert_eq!(
        workspace.run(ED25519_PUBLIC, &[], &mut out, &[0; 32]),
        Ok(32)
    );
    assert_eq!(workspace.stack_peaks(), &[0; OP_COUNT]); // host does not switch/measure stacks
    assert_eq!(storage.before, [0xa5; 16]);
    assert_eq!(storage.after, [0x5a; 16]);
}
