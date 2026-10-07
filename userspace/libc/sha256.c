/*
 * libc: SHA-256 (FIPS 180-4) and password hashes.
 */

#include <sha256.h>
#include <stdio.h>
#include <string.h>

#define PASSWORD_ROUNDS 10000

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr(uint32_t x, int n)
{
    return x >> n | x << (32 - n);
}

static void compress(sha256_t *s, const uint8_t *block)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 | (uint32_t)block[i * 4 + 2] << 8 |
               block[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->state[0], b = s->state[1], c = s->state[2], d = s->state[3];
    uint32_t e = s->state[4], f = s->state[5], g = s->state[6], h = s->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->state[0] += a;
    s->state[1] += b;
    s->state[2] += c;
    s->state[3] += d;
    s->state[4] += e;
    s->state[5] += f;
    s->state[6] += g;
    s->state[7] += h;
}

void sha256_init(sha256_t *s)
{
    static const uint32_t initial[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->state, initial, sizeof(initial));
    s->length = 0;
    s->used = 0;
}

void sha256_update(sha256_t *s, const void *data, size_t length)
{
    const uint8_t *p = data;
    s->length += length;
    while (length) {
        size_t n = 64 - s->used < length ? 64 - s->used : length;
        memcpy(s->block + s->used, p, n);
        s->used += n;
        p += n;
        length -= n;
        if (s->used == 64) {
            compress(s, s->block);
            s->used = 0;
        }
    }
}

void sha256_final(sha256_t *s, uint8_t digest[SHA256_DIGEST_SIZE])
{
    uint64_t bits = s->length * 8;
    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    pad = 0;
    while (s->used != 56)
        sha256_update(s, &pad, 1);
    uint8_t length[8];
    for (int i = 0; i < 8; i++)
        length[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(s, length, 8);
    for (int i = 0; i < 8; i++) {
        digest[i * 4] = (uint8_t)(s->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(s->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(s->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)s->state[i];
    }
}

void sha256(const void *data, size_t length, uint8_t digest[SHA256_DIGEST_SIZE])
{
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, data, length);
    sha256_final(&s, digest);
}

int password_hash(const char *salt, const char *password, char *out, size_t size)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, salt, strlen(salt));
    sha256_update(&s, password, strlen(password));
    sha256_final(&s, digest);
    for (int round = 1; round < PASSWORD_ROUNDS; round++) {
        sha256_init(&s);
        sha256_update(&s, digest, sizeof(digest));
        sha256_update(&s, password, strlen(password));
        sha256_final(&s, digest);
    }
    int n = snprintf(out, size, "sha256$%s$", salt);
    if (n < 0 || (size_t)n + 2 * SHA256_DIGEST_SIZE + 1 > size)
        return -1;
    for (int i = 0; i < SHA256_DIGEST_SIZE; i++)
        snprintf(out + n + 2 * i, 3, "%02x", digest[i]);
    return 0;
}

int password_check(const char *stored, const char *password)
{
    char salt[64], computed[PASSWORD_HASH_MAX];
    if (strncmp(stored, "sha256$", 7) != 0)
        return 0; /* "!" or "*": locked */
    const char *start = stored + 7, *end = strchr(start, '$');
    if (!end || (size_t)(end - start) >= sizeof(salt))
        return 0;
    memcpy(salt, start, (size_t)(end - start));
    salt[end - start] = '\0';
    if (password_hash(salt, password, computed, sizeof(computed)))
        return 0;
    /* Compare everything, so the time does not depend on where it differs. */
    size_t a = strlen(stored), b = strlen(computed);
    unsigned difference = (unsigned)(a ^ b);
    for (size_t i = 0; i < a && i < b; i++)
        difference |= (unsigned)(stored[i] ^ computed[i]);
    return difference == 0;
}
