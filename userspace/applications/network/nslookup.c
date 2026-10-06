/* nslookup NAME...: resolve host names to IPv4 addresses. */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    int status = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: nslookup NAME...\n");
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        struct hostent *entry = gethostbyname(argv[i]);
        if (!entry) {
            printf("%s: %s\n", argv[i], errno == ENOENT ? "no such name" : strerror(errno));
            status = 1;
            continue;
        }
        struct in_addr address;
        memcpy(&address, entry->h_addr, 4);
        printf("Name:    %s\nAddress: %s\n", argv[i], inet_ntoa(address));
    }
    return status;
}
