#!/usr/bin/env python3
"""Make a JellyOS password hash for /etc/passwd.

usage: mkpasswd.py PASSWORD [SALT]

Same scheme as password_hash() in userspace/libc/sha256.c:
"sha256$SALT$HEX", H1 = SHA256(SALT + password), Hn = SHA256(Hn-1 + password),
10000 rounds.
"""

import hashlib
import secrets
import sys

ROUNDS = 10000


def make(password, salt):
    digest = hashlib.sha256((salt + password).encode()).digest()
    for _ in range(ROUNDS - 1):
        digest = hashlib.sha256(digest + password.encode()).digest()
    return f"sha256${salt}${digest.hex()}"


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__.strip().splitlines()[2])
    print(make(sys.argv[1], sys.argv[2] if len(sys.argv) == 3 else secrets.token_hex(8)))
