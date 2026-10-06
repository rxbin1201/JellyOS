/*
 * ping [-c COUNT] [-i SECONDS] [-W SECONDS] HOST: ICMP echo through a ping socket.
 *
 * Exit code 0 if at least one reply arrived, 1 otherwise, 2 for usage errors.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <jelly/os.h>

#define PAYLOAD 56

static double parse_seconds(const char *text)
{
    double value = 0, scale = 1;
    int fraction = 0;
    for (; *text; text++) {
        if (*text == '.' && !fraction) {
            fraction = 1;
        } else if (*text >= '0' && *text <= '9') {
            if (fraction) {
                scale /= 10;
                value += (*text - '0') * scale;
            } else {
                value = value * 10 + (*text - '0');
            }
        } else {
            return -1;
        }
    }
    return value;
}

int main(int argc, char **argv)
{
    int count = 4;
    double interval = 1.0, wait = 2.0;
    const char *host = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)
            count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-i") && i + 1 < argc)
            interval = parse_seconds(argv[++i]);
        else if (!strcmp(argv[i], "-W") && i + 1 < argc)
            wait = parse_seconds(argv[++i]);
        else if (argv[i][0] != '-' && !host)
            host = argv[i];
        else
            host = NULL, i = argc;
    }
    if (!host || count <= 0 || interval < 0 || wait <= 0) {
        fprintf(stderr, "usage: ping [-c COUNT] [-i SECONDS] [-W SECONDS] HOST\n");
        return 2;
    }

    struct hostent *entry = gethostbyname(host);
    if (!entry) {
        fprintf(stderr, "ping: %s: %s\n", host, strerror(errno));
        return 2;
    }
    struct sockaddr_in to = { AF_INET, 0, { 0 }, { 0 } };
    memcpy(&to.sin_addr, entry->h_addr, 4);

    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (s < 0) {
        fprintf(stderr, "ping: socket: %s\n", strerror(errno));
        return 2;
    }
    printf("PING %s (%s): %d data bytes\n", host, inet_ntoa(to.sin_addr), PAYLOAD);
    fflush(stdout);

    int received = 0;
    uint64_t min = UINT64_MAX, max = 0, sum = 0;
    for (int sequence = 1; sequence <= count; sequence++) {
        uint8_t request[8 + PAYLOAD], reply[8 + PAYLOAD + 64];
        memset(request, 0, sizeof(request));
        request[0] = 8;
        request[6] = (uint8_t)(sequence >> 8);
        request[7] = (uint8_t)sequence;
        for (int i = 0; i < PAYLOAD; i++)
            request[8 + i] = (uint8_t)i;

        uint64_t sent = jelly_clock_ns();
        if (sendto(s, request, sizeof(request), 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
            printf("ping: send: %s\n", strerror(errno));
        } else {
            uint64_t deadline = sent + (uint64_t)(wait * 1e9);
            for (;;) {
                uint64_t now = jelly_clock_ns();
                if (now >= deadline) {
                    printf("Request timeout for icmp_seq %d\n", sequence);
                    break;
                }
                struct timeval timeout = { (time_t)((deadline - now) / 1000000000ULL),
                                           (long)((deadline - now) % 1000000000ULL / 1000) };
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                struct sockaddr_in from;
                socklen_t length = sizeof(from);
                ssize_t n = recvfrom(s, reply, sizeof(reply), 0, (struct sockaddr *)&from, &length);
                if (n < 0) {
                    if (errno != ETIMEDOUT)
                        printf("ping: %s\n", strerror(errno));
                    else
                        printf("Request timeout for icmp_seq %d\n", sequence);
                    break;
                }
                int reply_sequence = reply[6] << 8 | reply[7];
                if (n < 8 || reply[0] != 0 || reply_sequence != sequence)
                    continue; /* a late reply to an earlier request */
                uint64_t rtt = jelly_clock_ns() - sent;
                printf("%ld bytes from %s: icmp_seq=%d time=%lu.%03lu ms\n", (long)n, inet_ntoa(from.sin_addr),
                       sequence, (unsigned long)(rtt / 1000000), (unsigned long)(rtt / 1000 % 1000));
                received++;
                sum += rtt;
                if (rtt < min)
                    min = rtt;
                if (rtt > max)
                    max = rtt;
                break;
            }
        }
        fflush(stdout);
        if (sequence < count)
            usleep((uint64_t)(interval * 1e6));
    }

    printf("--- %s ping statistics ---\n", host);
    printf("%d packets transmitted, %d received, %d%% packet loss\n", count, received,
           (count - received) * 100 / count);
    if (received)
        printf("round-trip min/avg/max = %lu.%03lu/%lu.%03lu/%lu.%03lu ms\n", (unsigned long)(min / 1000000),
               (unsigned long)(min / 1000 % 1000), (unsigned long)(sum / received / 1000000),
               (unsigned long)(sum / received / 1000 % 1000), (unsigned long)(max / 1000000),
               (unsigned long)(max / 1000 % 1000));
    close(s);
    return received ? 0 : 1;
}
