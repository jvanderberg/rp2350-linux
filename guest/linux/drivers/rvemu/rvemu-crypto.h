/* /dev/rvemu-crypto (crypto-rvemu.c): the rvemu machine's native buffer
 * engine. Fixed-width RV32 ABI; addresses are the caller's. */
#ifndef RVEMU_CRYPTO_H
#define RVEMU_CRYPTO_H
#include <linux/types.h>
#include <linux/ioctl.h>
#define RVEMU_CRYPTO_MAX_BUFFER 32768
#define RVEMU_CRYPTO_MAX_PARAMS 4096
#define RVEMU_LZ4_DECODE 0
#define RVEMU_LZ4_ENCODE 1
#define RVEMU_CHACHA20POLY1305_SEAL 2
#define RVEMU_CHACHA20POLY1305_OPEN 3
#define RVEMU_SSH_PACKET_SEAL 4
#define RVEMU_SSH_PACKET_OPEN 5
#define RVEMU_X25519 6
#define RVEMU_ED25519_PUBLIC 7
#define RVEMU_ED25519_SIGN 8
#define RVEMU_ED25519_VERIFY 9
#define RVEMU_SNTRUP761_KEYPAIR 10
#define RVEMU_SNTRUP761_ENCAP 11
#define RVEMU_SNTRUP761_DECAP 12
struct rvemu_crypto_request {
    __u32 operation, source, source_len, destination, capacity;
    __u32 parameters, parameters_len, output_len;
    __u32 flags; /* must be zero */
};
#define RVEMU_CRYPTO_RUN _IOWR('R', 0x42, struct rvemu_crypto_request)
#endif
