/* Standalone non-SSH consumer of the generic accelerator ABI. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "rvemu-crypto.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"crypto probe failed at line %d errno=%d\n",__LINE__,errno); return 1; } } while(0)
static int call(int fd,unsigned op,const void *in,unsigned len,void *out,unsigned cap,const void *params,unsigned plen) {
    struct rvemu_crypto_request r={.operation=op,.source=(uintptr_t)in,.source_len=len,.destination=(uintptr_t)out,.capacity=cap,.parameters=(uintptr_t)params,.parameters_len=plen};
    if(ioctl(fd,RVEMU_CRYPTO_RUN,&r)) return -1;
    return r.output_len;
}
static void unhex(unsigned char *out,const char *in) { unsigned x; while(*in) { sscanf(in,"%2x",&x);*out++=x;in+=2; } }
int main(void) {
    unsigned char seed[32],pk[32],sig[64],params[96],out[64],expected[32];
    unsigned char kp[2921],enc[1071],shared[32];
    const char *message="firmware manifest: version 42";
    struct rvemu_crypto_request bad={0};
    int fd=open("/dev/rvemu-crypto",O_RDWR);
    CHECK(fd>=0);
    CHECK(ioctl(fd,0,&bad)==-1 && errno==ENOTTY);
    CHECK(ioctl(fd,RVEMU_CRYPTO_RUN,NULL)==-1 && errno==EFAULT);
    bad.flags=1; CHECK(ioctl(fd,RVEMU_CRYPTO_RUN,&bad)==-1 && errno==EINVAL);
    bad.flags=0;bad.parameters_len=4097; CHECK(ioctl(fd,RVEMU_CRYPTO_RUN,&bad)==-1 && errno==EMSGSIZE);
    CHECK(call(fd,99,NULL,0,NULL,0,NULL,0)==-1 && errno==EOPNOTSUPP);
    CHECK(call(fd,0,"\x50hello",6,out,sizeof(out),NULL,0)==5 && !memcmp(out,"hello",5));
    unhex(seed,"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    unhex(expected,"d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    CHECK(call(fd,RVEMU_ED25519_PUBLIC,NULL,0,pk,32,seed,32)==32 && !memcmp(pk,expected,32));
    CHECK(call(fd,RVEMU_ED25519_PUBLIC,NULL,0,NULL,32,seed,32)==-1 && errno==EFAULT);
    CHECK(call(fd,RVEMU_ED25519_SIGN,message,strlen(message),sig,64,seed,32)==64);
    memcpy(params,pk,32);memcpy(params+32,sig,64);
    CHECK(call(fd,RVEMU_ED25519_VERIFY,message,strlen(message),NULL,0,params,96)==0);
    params[40]^=1;memset(out,0xa5,sizeof(out));
    CHECK(call(fd,RVEMU_ED25519_VERIFY,message,strlen(message),out,sizeof(out),params,96)==-1 && errno==EBADMSG);
    for(unsigned i=0;i<sizeof(out);i++) CHECK(out[i]==0xa5);
    CHECK(call(fd,RVEMU_X25519,NULL,32,out,32,seed,32)==-1 && errno==EFAULT);
    memset(pk,0,32);memset(out,0xa5,32);
    CHECK(call(fd,RVEMU_X25519,pk,32,out,32,seed,32)==-1 && errno==EBADMSG);
    for(unsigned i=0;i<32;i++) CHECK(out[i]==0xa5);
    unhex(seed,"77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    /* Same user buffer for input/output is allowed; kernel owns disjoint copies. */
    pk[0]=9;CHECK(call(fd,RVEMU_X25519,pk,32,pk,32,seed,32)==32);
    unhex(expected,"8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    CHECK(!memcmp(pk,expected,32));
    memset(seed,17,32);CHECK(call(fd,RVEMU_SNTRUP761_KEYPAIR,NULL,0,kp,sizeof(kp),seed,32)==sizeof(kp));
    memset(seed,23,32);CHECK(call(fd,RVEMU_SNTRUP761_ENCAP,kp,1158,enc,sizeof(enc),seed,32)==sizeof(enc));
    CHECK(call(fd,RVEMU_SNTRUP761_DECAP,enc,1039,shared,32,kp+1158,1763)==32);
    CHECK(!memcmp(shared,enc+1039,32));enc[0]^=1;
    CHECK(call(fd,RVEMU_SNTRUP761_DECAP,enc,1039,shared,32,kp+1158,1763)==32);
    CHECK(memcmp(shared,enc+1039,32));
    close(fd);puts("GENERIC CRYPTO ABI PASS");return 0;
}
