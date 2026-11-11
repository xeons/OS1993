/*
 * ipconfig.c -- IPCONFIG.BIN: the network interface's settings.
 */
#include <stdio.h>
#include "os1993.h"
#include "winsock.h"

static void ip(const char *what, uint32_t v)
{
    printf("        %-28s: %lu.%lu.%lu.%lu\n", what, (unsigned long)(v >> 24), (unsigned long)(v >> 16 & 255),
           (unsigned long)(v >> 8 & 255), (unsigned long)(v & 255));
}

static int ipconfig_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct net_config c;
    printf("\nOS1993 IP Configuration\n\n");
    if (!GetNetworkConfig(&c)) {
        printf("The network can't be reached (error %d).\n", WSAGetLastError());
        return 1;
    }
    if (!c.up) {
        printf("No network card was found: only the loopback interface (127.0.0.1).\n");
        return 0;
    }
    printf("Ethernet adapter NE2000:\n\n");
    printf("        %-28s: %02X-%02X-%02X-%02X-%02X-%02X\n", "Physical Address", c.mac[0], c.mac[1],
           c.mac[2], c.mac[3], c.mac[4], c.mac[5]);
    printf("        %-28s: %s\n", "DHCP Enabled", "Yes");
    if (c.dhcp != 2) {
        printf("        %-28s: %s\n", "IP Address", "(asking DHCP)");
        return 0;
    }
    ip("IP Address", c.ip);
    ip("Subnet Mask", c.mask);
    ip("Default Gateway", c.gateway);
    ip("DHCP Server", c.dhcp_server);
    ip("DNS Server", c.dns);
    printf("        %-28s: %lu seconds\n", "Lease", (unsigned long)c.lease_secs);
    printf("        %-28s: %lu received, %lu sent, %lu dropped\n", "Packets", (unsigned long)c.rx_packets,
           (unsigned long)c.tx_packets, (unsigned long)(c.rx_dropped + c.tx_dropped));
    return 0;
}

int main(int argc, char **argv)
{
    return RunConsoleProgram("IP Configuration", ipconfig_main, argc, argv);
}
