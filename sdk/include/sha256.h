/*
 * JellyOS libc: SHA-256 (FIPS 180-4) and password hashes.
 */

#ifndef _SHA256_H
#define _SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t length;      /* bytes processed */
    uint8_t  block[64];
    size_t   used;
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, size_t length);
void sha256_final(sha256_t *s, uint8_t digest[SHA256_DIGEST_SIZE]);
void sha256(const void *data, size_t length, uint8_t digest[SHA256_DIGEST_SIZE]);

/*
 * Password hashes as stored in /etc/passwd: "sha256$SALT$HEX" where
 * HEX = H^10000, H1 = SHA256(SALT || password), Hn = SHA256(Hn-1 || password).
 * tools/image_builder/mkpasswd.py makes the same hashes on the host.
 */
#define PASSWORD_HASH_MAX 128
int  password_hash(const char *salt, const char *password, char *out, size_t size);
/* 1 if password matches the stored hash, 0 otherwise (also for "!" = locked). */
int  password_check(const char *stored, const char *password);

#endif
