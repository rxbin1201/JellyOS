/*
 * Host unit tests for userspace/libc/sha256.c: FIPS 180-4 test vectors and
 * the password hash scheme shared with tools/image_builder/mkpasswd.py.
 */

#include <stdio.h>
#include <string.h>

#include <sha256.h>

static int failures, checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            printf("unit: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                        \
    } while (0)

static int digest_is(const char *text, const char *hex)
{
    uint8_t digest[32];
    char out[65];
    sha256(text, strlen(text), digest);
    for (int i = 0; i < 32; i++)
        snprintf(out + 2 * i, 3, "%02x", digest[i]);
    return strcmp(out, hex) == 0;
}

int main(void)
{
    CHECK(digest_is("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(digest_is("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK(digest_is("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    /* A million 'a' fed in odd chunks */
    sha256_t s;
    uint8_t digest[32], chunk[997];
    memset(chunk, 'a', sizeof(chunk));
    sha256_init(&s);
    size_t left = 1000000;
    while (left) {
        size_t n = left < sizeof(chunk) ? left : sizeof(chunk);
        sha256_update(&s, chunk, n);
        left -= n;
    }
    sha256_final(&s, digest);
    CHECK(digest[0] == 0xcd && digest[1] == 0xc7 && digest[31] == 0xd0);

    /* Same result as mkpasswd.py jelly 4a656c6c794f5321 (the default account) */
    char hash[PASSWORD_HASH_MAX];
    CHECK(password_hash("4a656c6c794f5321", "jelly", hash, sizeof(hash)) == 0);
    CHECK(!strcmp(hash, "sha256$4a656c6c794f5321$363bb4ddabccb8f929a0084b08bd7f045d07a1de585ddab09490b803aa40cb43"));
    CHECK(password_check(hash, "jelly") == 1);
    CHECK(password_check(hash, "Jelly") == 0);
    CHECK(password_check(hash, "") == 0);
    CHECK(password_check("!", "") == 0);
    CHECK(password_check("sha256$broken", "jelly") == 0);

    printf("unit: sha256 %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
