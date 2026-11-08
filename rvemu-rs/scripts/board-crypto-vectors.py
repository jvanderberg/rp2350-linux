#!/usr/bin/env python3
"""Write the native crypto vector file the board guest's `crypto-vectors`
runs through /dev/rvemu-crypto: independent RFC 7748 / RFC 8032 / RFC 8439 /
SSH draft / sntrup761 draft vectors, largest legal buffers, tampering and
malformed requests. Expected outputs that have no published vector (seeded
KEM keypair/encapsulation, maximum-size messages) come from the host build of
the same engine (rvaccel's `transform` example), so the board's RV32 run is
compared with an independent x86/ARM build of it.

Usage: scripts/board-crypto-vectors.py OUT_FILE
"""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Linux errno values the kernel adapter returns (rvemu-crypto.c)
EBADMSG, EOPNOTSUPP, EINVAL, EMSGSIZE = 74, 95, 22, 90


def transform(op, src, params):
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        (d / 'in').write_bytes(src)
        (d / 'p').write_bytes(params)
        subprocess.run([str(ROOT / 'target/release/examples/transform'), str(op), str(d / 'in'), str(d / 'out'), str(d / 'p')], check=True)
        return (d / 'out').read_bytes()


def main(out):
    subprocess.run(['cargo', 'build', '-q', '--release', '-p', 'rvaccel', '--example', 'transform'], cwd=ROOT, check=True)
    rows = []

    def case(name, op, src, params, expected=b'', err=0, cap=None):
        cap = len(expected) if cap is None else cap
        rows.append((name, op, err, cap, src, params, expected))

    h = bytes.fromhex
    # RFC 8032 7.1 test 1, RFC 7748 6.1
    seed = h('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')
    pk = h('d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a')
    sig = h('e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b')
    xsecret = h('77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a')
    xpub = h('8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a')
    bsecret = h('5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb')
    bpub = h('de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f')
    shared = h('4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742')
    case('x25519-rfc7748-alice-public', 6, b'\x09' + bytes(31), xsecret, xpub)
    case('x25519-rfc7748-bob-public', 6, b'\x09' + bytes(31), bsecret, bpub)
    case('x25519-rfc7748-shared', 6, bpub, xsecret, shared)
    case('x25519-low-order-rejected', 6, bytes(32), xsecret, err=EBADMSG, cap=32)
    case('ed25519-rfc8032-public', 7, b'', seed, pk)
    case('ed25519-rfc8032-sign', 8, b'', seed, sig)
    case('ed25519-rfc8032-verify', 9, b'', pk + sig)
    case('ed25519-bad-signature-rejected', 9, b'changed', pk + sig, err=EBADMSG, cap=32)
    bad = bytearray(sig)
    bad[40] ^= 1
    case('ed25519-tampered-signature-rejected', 9, b'', pk + bytes(bad), err=EBADMSG)
    # RFC 8439 2.8.2
    plain = b"Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it."
    aparams = bytes(range(0x80, 0xa0)) + h('07000000404142434445464750515253c0c1c2c3c4c5c6c7')
    wire = h('d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b61161ae10b594f09e26a7e902ecbd0600691')
    case('aead-rfc8439-seal', 2, plain, aparams, wire)
    case('aead-rfc8439-open', 3, wire, aparams, plain)
    for i in range(len(wire)):
        t = bytearray(wire)
        t[i] ^= 1
        case(f'aead-rfc8439-tamper-{i}', 3, bytes(t), aparams, err=EBADMSG, cap=len(plain))
    case('aead-short-input-invalid', 3, wire[:15], aparams, err=EINVAL, cap=len(plain))
    case('aead-short-params-invalid', 2, plain, aparams[:43], err=EINVAL, cap=len(wire))
    # draft-ietf-sshm-chacha20-poly1305-04 appendix A
    sp = h('00000048065e00000000000000384c6f72656d20697073756d20646f6c6f722073697420616d65742c20636f6e7365637465747572206164697069736963696e6720656c69744e43e804dc6c')
    sk = h('8bbff6855fc102338c373e73aac0c914f076a905b2444a32eecaffeae22becc5e9b7a7a5825a8249346ec1c28301cf394543fc7569887d76e168f37562ac07400000000000000007')
    sw = h('2c3ecce4a5bc05895bf07a7ba956b6c68829ac7c83b780b7000ecde745afc705bbc378ce03a280236b87b53bed5839662302b164b6286a48cd1e097138e3cb909b8b2b829dd18d2a35ff82d995349e855bf02c298ef775f2d1a7e8b8')
    case('ssh-draft-seal', 4, sp, sk, sw)
    case('ssh-draft-open', 5, sw, sk, sp)
    for i in range(len(sw)):
        t = bytearray(sw)
        t[i] ^= 1
        case(f'ssh-draft-tamper-{i}', 5, bytes(t), sk, err=EBADMSG, cap=len(sw))
    # sntrup761 draft-josefsson-ntruprime-streamlined-00 vectors 0 and 1
    for i in (0, 1):
        def fixture(n):
            return h((ROOT / f'rvaccel/tests/data/sntrup761-kat{i}-{n}.hex').read_text().strip())
        case(f'sntrup761-ietf-decap-{i}', 12, fixture('ct'), fixture('sk'), fixture('ss'))
    kp = transform(10, b'', bytes([17]) * 32)
    enc = transform(11, kp[:1158], bytes([23]) * 32)
    case('sntrup761-keypair-seeded', 10, b'', bytes([17]) * 32, kp)
    case('sntrup761-encap-seeded', 11, kp[:1158], bytes([23]) * 32, enc)
    case('sntrup761-decap-seeded', 12, enc[:1039], kp[1158:], enc[1039:])
    badct = bytes([enc[0] ^ 1]) + enc[1:1039]
    rej = transform(12, badct, kp[1158:])
    assert rej != enc[1039:]
    case('sntrup761-implicit-rejection', 12, badct, kp[1158:], rej)
    case('sntrup761-short-key-invalid', 12, enc[:1039], kp[1158:-1], err=EINVAL, cap=32)
    # LZ4
    case('lz4-decode-hello', 0, b'\x50hello', b'', b'hello')
    case('lz4-decode-truncated-rejected', 0, b'\xf0', b'', err=EBADMSG, cap=64)
    case('lz4-decode-small-output-rejected', 0, b'\x50hello', b'', err=EBADMSG, cap=4)
    case('lz4-params-invalid', 0, b'\x50hello', b'\x00', err=EINVAL, cap=5)
    compressible = bytes(i % 31 for i in range(29770))
    packed = transform(1, compressible, b'')
    case('lz4-encode-large', 1, compressible, b'', packed, cap=32768)
    case('lz4-decode-large', 0, packed, b'', compressible)
    ext = 32768 - 25
    block = b'\x1f\x00\x01\x00' + b'\xff' * (ext // 255) + bytes([ext % 255]) + b'\x50' + bytes(5)
    case('lz4-decode-max', 0, block, b'', bytes(32768))
    # Largest legal messages
    big = bytes(i % 251 for i in range(32752))
    bparams = bytes(range(32)) + bytes(range(12)) + bytes(1024)
    bwire = transform(2, big, bparams)
    case('aead-seal-max', 2, big, bparams, bwire)
    case('aead-open-max', 3, bwire, bparams, big)
    case('aead-bad-tag-max-rejected', 3, bwire[:-1] + bytes([bwire[-1] ^ 1]), bparams, err=EBADMSG, cap=len(big))
    packet = (32748).to_bytes(4, 'big') + big[:32748]
    sshp = bytes(range(64)) + (7).to_bytes(8, 'big')
    swire = transform(4, packet, sshp)
    case('ssh-seal-max', 4, packet, sshp, swire)
    case('ssh-open-max', 5, swire, sshp, packet)
    msg = bytes(i % 251 for i in range(32768))
    msig = transform(8, msg, seed)
    case('ed25519-sign-max', 8, msg, seed, msig)
    case('ed25519-verify-max', 9, msg, pk + msig)
    # Unknown operation, through the kernel and SBI
    case('unknown-operation', 13, b'', b'', err=EOPNOTSUPP)
    assert {r[1] for r in rows} >= set(range(13))
    # every successful case must also hold on the host build, so a mistyped
    # vector fails here and not on the board
    for name, op, err, cap, src, params, expected in rows:
        if not err:
            got = transform(op, src, params)
            assert got == expected, f'{name}: the host engine disagrees with the vector'

    blob = bytearray(b'RVCV' + struct.pack('<I', len(rows)))
    for name, op, err, cap, src, params, expected in rows:
        n = name.encode()
        assert len(n) < 48
        blob += n.ljust(48, b'\0') + struct.pack('<6I', op, err, cap, len(src), len(params), len(expected))
        blob += src + params + expected
    Path(out).write_bytes(blob)
    print(f'{out}: {len(rows)} cases, {len(blob)} bytes')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
