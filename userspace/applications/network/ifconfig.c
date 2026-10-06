/*
 * ifconfig: show or configure network interfaces.
 *
 *   ifconfig                                  all interfaces
 *   ifconfig NAME                             one interface
 *   ifconfig NAME ADDRESS/PREFIX [gateway G] [dns D]   static configuration (root)
 *   ifconfig NAME down                        remove the address (root)
 */

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jelly/os.h>

static const char *text_of(uint32_t address, char *buffer)
{
    struct in_addr a = { address };
    return inet_ntop(AF_INET, &a, buffer, INET_ADDRSTRLEN);
}

static void show(const jelly_netif_info_t *info)
{
    char a[INET_ADDRSTRLEN], b[INET_ADDRSTRLEN];

    printf("%s: %s%s%s mtu %u\n", info->name, (info->flags & JELLY_NETIF_UP) ? "UP" : "DOWN",
           (info->flags & JELLY_NETIF_LOOPBACK) ? " LOOPBACK" : "",
           (info->flags & JELLY_NETIF_LINK) ? " RUNNING" : " NO-CARRIER", info->mtu);
    if (!(info->flags & JELLY_NETIF_LOOPBACK))
        printf("    ether %02x:%02x:%02x:%02x:%02x:%02x\n", info->mac[0], info->mac[1], info->mac[2], info->mac[3],
               info->mac[4], info->mac[5]);
    if (info->address) {
        printf("    inet %s/%d", text_of(info->address, a), __builtin_popcount(ntohl(info->netmask)));
        if (info->gateway)
            printf(" gateway %s", text_of(info->gateway, b));
        if (info->dns)
            printf(" dns %s", text_of(info->dns, b));
        printf("\n");
    } else {
        printf("    inet (not configured)\n");
    }
    printf("    RX %lu packets %lu bytes %lu dropped\n", (unsigned long)info->rx_packets,
           (unsigned long)info->rx_bytes, (unsigned long)info->rx_dropped);
    printf("    TX %lu packets %lu bytes %lu dropped\n", (unsigned long)info->tx_packets,
           (unsigned long)info->tx_bytes, (unsigned long)info->tx_dropped);
}

static int find(const char *name, jelly_netif_info_t *info)
{
    for (uint32_t i = 0; !jelly_net_interface_info(i, info); i++) {
        if (!strcmp(info->name, name))
            return 0;
    }
    fprintf(stderr, "ifconfig: %s: no such interface\n", name);
    return -1;
}

static int parse_address(const char *text, uint32_t *address)
{
    struct in_addr a;
    if (!inet_aton(text, &a)) {
        fprintf(stderr, "ifconfig: bad address '%s'\n", text);
        return -1;
    }
    *address = a.s_addr;
    return 0;
}

static int configure(const jelly_netif_info_t *info, int argc, char **argv)
{
    jelly_netif_config_t config = { 0, 0, 0, 0 };

    if (strcmp(argv[0], "down") != 0) {
        char address[32];
        snprintf(address, sizeof(address), "%s", argv[0]);
        char *slash = strchr(address, '/');
        long prefix = 24;
        if (slash) {
            *slash = '\0';
            prefix = strtol(slash + 1, NULL, 10);
        }
        if (prefix < 1 || prefix > 32 || parse_address(address, &config.address))
            return 2;
        config.netmask = htonl(prefix == 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> prefix));
        for (int i = 1; i + 1 < argc; i += 2) {
            if (!strcmp(argv[i], "gateway") && !parse_address(argv[i + 1], &config.gateway))
                continue;
            if (!strcmp(argv[i], "dns") && !parse_address(argv[i + 1], &config.dns))
                continue;
            fprintf(stderr, "ifconfig: unexpected '%s'\n", argv[i]);
            return 2;
        }
    }
    status_t status = jelly_net_configure(info->index, &config);
    if (STATUS_IS_ERROR(status)) {
        fprintf(stderr, "ifconfig: %s: %s\n", info->name, strerror((int)status));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    jelly_netif_info_t info;

    if (argc == 1) {
        for (uint32_t i = 0; !jelly_net_interface_info(i, &info); i++) {
            if (i)
                printf("\n");
            show(&info);
        }
        return 0;
    }
    if (find(argv[1], &info))
        return 1;
    if (argc == 2) {
        show(&info);
        return 0;
    }
    return configure(&info, argc - 2, argv + 2);
}
