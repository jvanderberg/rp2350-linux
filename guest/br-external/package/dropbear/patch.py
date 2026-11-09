#!/usr/bin/env python3
"""Dropbear 2026.94 on the rvemu native engine: exact, idempotent edits that
hook the rvemu-*.inc adapters into its source. Usage: patch.py SOURCE_DIR"""
from pathlib import Path
import sys
root=Path(sys.argv[1])
p=root/'src/chachapoly.h';s=p.read_text()
if 'rvemu_key' not in s:
    needle='\tchacha_state header;'
    assert s.count(needle)==1
    p.write_text(s.replace(needle,needle+'\n\tunsigned char rvemu_key[64];'))
p=root/'src/chachapoly.c';s=p.read_text()
if 'rvemu-offload.inc' not in s:
    needle='#define POLY1305_TAG_LEN 16'
    assert s.count(needle)==1
    s=s.replace(needle,needle+'\n\n#include "rvemu-offload.inc"')
    needle='\tif ((err = chacha_setup(&state->chacha, key,'
    assert s.count(needle)==1
    s=s.replace(needle,'\tmemcpy(state->rvemu_key, key, 64);\n\n'+needle)
    needle='\tSTORE64H((uint64_t)seq, seqbuf);'
    assert s.count(needle)==2
    s=s.replace(needle,'\terr = rvemu_ssh_packet(seq, in, out, len, state->rvemu_key, direction);\n\tif (err) return err > 0 ? CRYPT_OK : CRYPT_ERROR;\n\n'+needle,1)
    p.write_text(s)

# Benchmark TCP connect -> initial key exchange -> authenticated connection.
# The emulator stores counters at these boundaries, printing them after exit.
edits = {
    'cli-main.c': ('\t\tprogress = connect_remote(', '\t\trvemu_benchmark(0);\n\t\tprogress = connect_remote('),
    'cli-session.c': ('\tmyses->sock_in = myses->sock_out = sock;', '\tmyses->sock_in = myses->sock_out = sock;\n\trvemu_benchmark(1);'),
    'common-kex.c': ('\tses.kexstate.recvfirstnewkeys = 1;', '\tif (IS_DROPBEAR_CLIENT && !ses.kexstate.recvfirstnewkeys) rvemu_benchmark(2);\n\tses.kexstate.recvfirstnewkeys = 1;'),
    'cli-auth.c': ('\tses.authstate.authdone = 1;', '\trvemu_benchmark(3);\n\tses.authstate.authdone = 1;'),
}
for name, (needle, replacement) in edits.items():
    p=root/'src'/name; s=p.read_text()
    if 'rvemu-bench.h' not in s:
        assert s.count(needle)==1, name
        s='#include "rvemu-bench.h"\n'+s.replace(needle,replacement)
        if name=='cli-main.c':
            needle='#include "runopts.h"'
            assert s.count(needle)==1
            s=s.replace(needle,needle+'\n#include "rvemu-bench.inc"')
        p.write_text(s)

# Faster packet processing exposes a client shutdown stall: channelio can
# drain the final buffered output and remove the last channel AFTER the loop
# handler has checked for completion. With nothing left to watch, the next
# select sleeps until the eight-hour rekey timeout. Notify the existing
# client state machine immediately on this transition (also in software mode).
p=root/'src/common-session.c';s=p.read_text()
if 'rvemu final channel completion' not in s:
    needle='\t\tchannelio(&readfd, &writefd);'
    assert s.count(needle)==1
    s=s.replace(needle,'''\t\t/* rvemu final channel completion */
\t\t{
\t\t\tunsigned int previous_channels = ses.chancount;
\t\t\tchannelio(&readfd, &writefd);
\t\t\tif (IS_DROPBEAR_CLIENT && previous_channels > 0 && ses.chancount == 0) {
\t\t\t\tloophandler();
\t\t\t}
\t\t}''')
    p.write_text(s)

# Protocol-independent operations through the generic accelerator device.
p=root/'src/curve25519.c'; s=p.read_text()
if 'rvemu-crypto.inc' not in s:
    s=s.replace('#include "curve25519.h"','#include "curve25519.h"\n#include "rvemu-crypto.inc"')
    def entry(signature, code):
        global s
        needle=signature+'\n{'
        assert s.count(needle)==1, signature
        s=s.replace(needle,needle+'\n'+code)
    entry('void dropbear_curve25519_scalarmult(u8 *q,const u8 *n,const u8 *p)', '''  {
    int rc=rvemu_crypto(RVEMU_X25519,p,32,q,32,n,32);
    if (rc<0) dropbear_exit("Native X25519 failed");
    if (rc>0) return;
  }''')
    needle='  genrandom(sk, 32);'
    assert s.count(needle)==1
    s=s.replace(needle,needle+'''
  {
    int rc=rvemu_crypto(RVEMU_ED25519_PUBLIC,NULL,0,pk,32,sk,32);
    if (rc<0) dropbear_exit("Native Ed25519 public key failed");
    if (rc>0) return;
  }''')
    entry('void dropbear_ed25519_sign(const u8 *m,u32 mlen,u8 *s,u32 *slen,const u8 *sk, const u8 *pk)', '''  {
    int rc=rvemu_crypto(RVEMU_ED25519_SIGN,m,mlen,s,64,sk,32);
    if (rc<0) dropbear_exit("Native Ed25519 signing failed");
    if (rc>0) { *slen=64; return; }
  }''')
    entry('int dropbear_ed25519_verify(const u8 *m,u32 mlen,const u8 *s,u32 slen,const u8 *pk)', '''  {
    unsigned char params[96];
    int rc;
    if (slen<64) return -1;
    memcpy(params,pk,32); memcpy(params+32,s,64);
    rc=rvemu_crypto(RVEMU_ED25519_VERIFY,m,mlen,NULL,0,params,96);
    m_burn(params,sizeof(params));
    if (rc) return rc>0 ? 0 : -1;
  }''')
    p.write_text(s)
p=root/'src/sntrup761.c'; s=p.read_text()
if 'rvemu-crypto.inc' not in s:
    needle='#include "sntrup761_compat.h"'
    s=s.replace(needle,needle+'\n#include "rvemu-crypto.inc"')
    def entry(signature, code):
        global s
        needle=signature+' {'
        assert s.count(needle)==1, signature
        s=s.replace(needle,needle+'\n'+code)
    entry('int crypto_kem_sntrup761_keypair(unsigned char *pk, unsigned char *sk)', '''  if (rvemu_crypto_fd()>=0) {
    unsigned char seed[32], output[1158+1763];
    int rc;
    genrandom(seed,sizeof(seed));
    rc=rvemu_crypto(RVEMU_SNTRUP761_KEYPAIR,NULL,0,output,sizeof(output),seed,sizeof(seed));
    if (rc>0) { memcpy(pk,output,1158); memcpy(sk,output+1158,1763); }
    m_burn(seed,sizeof(seed)); m_burn(output,sizeof(output));
    if (rc<0) dropbear_exit("Native sntrup761 keypair failed");
    if (rc>0) return 0;
  }''')
    entry('int crypto_kem_sntrup761_enc(unsigned char *c, unsigned char *k, const unsigned char *pk)', '''  if (rvemu_crypto_fd()>=0) {
    unsigned char seed[32], output[1039+32];
    int rc;
    genrandom(seed,sizeof(seed));
    rc=rvemu_crypto(RVEMU_SNTRUP761_ENCAP,pk,1158,output,sizeof(output),seed,sizeof(seed));
    if (rc>0) { memcpy(c,output,1039); memcpy(k,output+1039,32); }
    m_burn(seed,sizeof(seed)); m_burn(output,sizeof(output));
    if (rc<0) dropbear_exit("Native sntrup761 encapsulation failed");
    if (rc>0) return 0;
  }''')
    entry('int crypto_kem_sntrup761_dec(unsigned char *k, const unsigned char *c, const unsigned char *sk)', '''  {
    int rc=rvemu_crypto(RVEMU_SNTRUP761_DECAP,c,1039,k,32,sk,1763);
    if (rc<0) dropbear_exit("Native sntrup761 decapsulation failed");
    if (rc>0) return 0;
  }''')
    p.write_text(s)
