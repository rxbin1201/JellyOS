/*
 * nc: netcat.
 *
 *   nc [-u] HOST PORT     connect (TCP, or UDP with -u)
 *   nc -l [-u] PORT       listen; TCP accepts one connection
 *   -w SECONDS            give up connecting / waiting for data after that long
 *
 * stdin goes to the network and the network to stdout. A second thread
 * copies stdin, so both directions flow at once. At the end of stdin a TCP
 * connection is half closed; nc exits once the peer closes its side.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <threads.h>
#include <unistd.h>

static int udp, listening;
static int connection = -1;
static struct sockaddr_in peer;
static int peer_known;

static int send_stdin(void *arg)
{
    (void)arg;
    char buffer[4096];
    for (;;) {
        ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (n <= 0)
            break;
        ssize_t sent = udp ? sendto(connection, buffer, (size_t)n, 0, (struct sockaddr *)&peer, sizeof(peer))
                           : send(connection, buffer, (size_t)n, 0);
        if (sent < 0) {
            fprintf(stderr, "nc: send: %s\n", strerror(errno));
            return 1;
        }
    }
    if (!udp)
        shutdown(connection, SHUT_WR);
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: nc [-u] [-w SECONDS] HOST PORT | nc -l [-u] PORT\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *arguments[2] = { NULL, NULL };
    int count = 0, wait_seconds = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-u"))
            udp = 1;
        else if (!strcmp(argv[i], "-l"))
            listening = 1;
        else if (!strcmp(argv[i], "-w") && i + 1 < argc)
            wait_seconds = atoi(argv[++i]);
        else if (argv[i][0] != '-' && count < 2)
            arguments[count++] = argv[i];
        else
            usage();
    }
    if (count != (listening ? 1 : 2))
        usage();

    int port = atoi(arguments[count - 1]);
    if (port <= 0 || port > 65535)
        usage();
    int s = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (s < 0) {
        fprintf(stderr, "nc: socket: %s\n", strerror(errno));
        return 1;
    }
    if (wait_seconds > 0) {
        struct timeval timeout = { wait_seconds, 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }

    if (listening) {
        int one = 1;
        struct sockaddr_in local = { AF_INET, htons((uint16_t)port), { INADDR_ANY }, { 0 } };
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(s, (struct sockaddr *)&local, sizeof(local)) || (!udp && listen(s, 1))) {
            fprintf(stderr, "nc: port %d: %s\n", port, strerror(errno));
            return 1;
        }
        if (udp) {
            connection = s;
        } else {
            socklen_t length = sizeof(peer);
            connection = accept(s, (struct sockaddr *)&peer, &length);
            if (connection < 0) {
                fprintf(stderr, "nc: accept: %s\n", strerror(errno));
                return 1;
            }
            peer_known = 1;
            close(s);
        }
    } else {
        struct hostent *entry = gethostbyname(arguments[0]);
        if (!entry) {
            fprintf(stderr, "nc: %s: %s\n", arguments[0], strerror(errno));
            return 1;
        }
        peer.sin_family = AF_INET;
        peer.sin_port = htons((uint16_t)port);
        memcpy(&peer.sin_addr, entry->h_addr, 4);
        peer_known = 1;
        if (!udp && connect(s, (struct sockaddr *)&peer, sizeof(peer))) {
            fprintf(stderr, "nc: connect to %s port %d: %s\n", arguments[0], port, strerror(errno));
            return 1;
        }
        connection = s;
    }

    thrd_t sender;
    int sender_started = 0;
    if (peer_known)
        sender_started = thrd_create(&sender, send_stdin, NULL) == thrd_success;

    char buffer[4096];
    int status = 0;
    for (;;) {
        struct sockaddr_in from;
        socklen_t length = sizeof(from);
        ssize_t n = udp ? recvfrom(connection, buffer, sizeof(buffer), 0, (struct sockaddr *)&from, &length)
                        : recv(connection, buffer, sizeof(buffer), 0);
        if (n < 0) {
            if (errno != ETIMEDOUT) {
                fprintf(stderr, "nc: %s\n", strerror(errno));
                status = 1;
            }
            break;
        }
        if (n == 0 && !udp)
            break;
        /* A UDP listener answers whoever sent the first datagram. */
        if (udp && !peer_known) {
            peer = from;
            peer_known = 1;
            sender_started = thrd_create(&sender, send_stdin, NULL) == thrd_success;
        }
        fwrite(buffer, 1, (size_t)n, stdout);
        fflush(stdout);
    }
    /* The peer is done. The sender may still wait for console input: exiting ends it. */
    (void)sender_started;
    fflush(stdout);
    close(connection);
    exit(status);
}
