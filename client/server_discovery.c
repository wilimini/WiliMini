#include "server_discovery.h"
#include <pspkernel.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>

static void send_to(int fd, const unsigned char *data, int length, uint32_t address, int port) {
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_len = sizeof(to);
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)port);
    to.sin_addr.s_addr = address;
    sceNetInetSendto(fd, data, (size_t)length, 0, (struct sockaddr *)&to, sizeof(to));
}

/* The subnet's directed broadcast, as some access points drop 255.255.255.255. */
static uint32_t directed_broadcast(void) {
    SceNetApctlInfo info;
    struct in_addr ip, mask;
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info) < 0 || inet_aton(info.ip, &ip) != 1) return 0;
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_SUBNETMASK, &info) < 0 || inet_aton(info.subNetMask, &mask) != 1) return 0;
    return ip.s_addr | ~mask.s_addr;
}

static void remember(WiliFoundServer *out, int *count, int max, const char *host, int port, const char *name) {
    for (int i = 0; i < *count; ++i) {
        if (!strcmp(out[i].host, host) && out[i].port == port) {
            if (name[0] && !out[i].name[0]) snprintf(out[i].name, sizeof(out[i].name), "%s", name);
            return;
        }
    }
    if (*count >= max) return;
    snprintf(out[*count].host, sizeof(out[*count].host), "%s", host);
    out[*count].port = port;
    snprintf(out[*count].name, sizeof(out[*count].name), "%s", name);
    ++*count;
}

int wili_discover_servers(WiliFoundServer *out, int max, int timeout_ms, volatile int *cancel) {
    if (!out || max <= 0) return -1;
    int fd = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int on = 1;
    sceNetInetSetsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_len = sizeof(local);
    local.sin_family = AF_INET;
    if (sceNetInetBind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        sceNetInetClose(fd);
        return -1;
    }
    unsigned char mdns[64], beacon[32];
    const int mdns_length = wili_mdns_query(mdns, sizeof(mdns));
    const int beacon_length = wili_beacon_probe(beacon, sizeof(beacon));
    const uint32_t directed = directed_broadcast();

    int count = 0;
    const SceInt64 start = sceKernelGetSystemTimeWide();
    const SceInt64 deadline = start + (SceInt64)timeout_ms * 1000;
    SceInt64 next_send = start;
    int sends = 0;
    while (sceKernelGetSystemTimeWide() < deadline && count < max && !(cancel && *cancel)) {
        if (sceKernelGetSystemTimeWide() >= next_send && sends < 3) {
            send_to(fd, mdns, mdns_length, inet_addr("224.0.0.251"), 5353);
            send_to(fd, beacon, beacon_length, 0xFFFFFFFFu, WILI_BEACON_PORT);
            if (directed) send_to(fd, beacon, beacon_length, directed, WILI_BEACON_PORT);
            ++sends;
            next_send += 600000;
        }
        SceNetInetPollfd wait = {.fd = fd, .events = SCE_NET_INET_POLLIN};
        if (sceNetInetPoll(&wait, 1, 100) <= 0) continue;
        unsigned char packet[1500];
        struct sockaddr_in from;
        socklen_t from_length = sizeof(from);
        int bytes = (int)sceNetInetRecvfrom(fd, packet, sizeof(packet), 0, (struct sockaddr *)&from, &from_length);
        if (bytes <= 0) continue;
        char host[16], name[WILI_SERVER_NAME_MAX] = "";
        int port = 0;
        if (!inet_ntop(AF_INET, &from.sin_addr, host, sizeof(host))) continue;
        if (wili_mdns_parse(packet, (size_t)bytes, &port, name, sizeof(name)) == 0 ||
            wili_beacon_parse(packet, (size_t)bytes, &port, name, sizeof(name)) == 0)
            remember(out, &count, max, host, port, name);
    }
    sceNetInetClose(fd);
    return count;
}
