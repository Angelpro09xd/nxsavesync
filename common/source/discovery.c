#include "discovery.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Lee una cadena [u16 len][bytes] del buffer. Devuelve false si no cabe.
static bool take_str(const u8 *buf, size_t len, size_t *pos, char *out, size_t out_size)
{
    if (*pos + 2 > len) return false;
    size_t n = (size_t)buf[*pos] | ((size_t)buf[*pos + 1] << 8);
    *pos += 2;

    if (*pos + n > len) return false;

    size_t copy = n < out_size - 1 ? n : out_size - 1;
    memcpy(out, buf + *pos, copy);
    out[copy] = '\0';
    *pos += n;
    return true;
}

static bool already_known(hostlist_t *l, const char *ip)
{
    for (size_t i = 0; i < l->n; i++)
        if (!strcmp(l->v[i].ip, ip)) return true;
    return false;
}

bool discovery_scan(hostlist_t *out, int timeout_ms)
{
    memset(out, 0, sizeof(*out));

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;

    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) < 0) {
        close(fd);
        return false;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = 200 * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Sonda: "NXSS?" + version
    u8 probe[16];
    size_t plen = strlen(PROTO_DISC_PROBE);
    memcpy(probe, PROTO_DISC_PROBE, plen);
    for (int i = 0; i < 4; i++) probe[plen + i] = (u8)(PROTO_VERSION >> (8 * i));
    plen += 4;

    struct sockaddr_in bcast;
    memset(&bcast, 0, sizeof(bcast));
    bcast.sin_family      = AF_INET;
    bcast.sin_port        = htons(PROTO_DISC_PORT);
    bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    sendto(fd, probe, plen, 0, (struct sockaddr *)&bcast, sizeof(bcast));

    // Recogemos respuestas hasta agotar el tiempo. Repetimos la sonda a mitad
    // por si el primer datagrama se perdio, que en wifi pasa.
    u64 start = armGetSystemTick();
    u64 limit = armNsToTicks((u64)timeout_ms * 1000000ULL);
    bool resent = false;

    while (armGetSystemTick() - start < limit && out->n < DISC_MAX_HOSTS) {
        if (!resent && armGetSystemTick() - start > limit / 2) {
            sendto(fd, probe, plen, 0, (struct sockaddr *)&bcast, sizeof(bcast));
            resent = true;
        }

        u8 buf[512];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);

        ssize_t k = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (k <= 0) continue;

        size_t rlen = strlen(PROTO_DISC_REPLY);
        if ((size_t)k < rlen + 4 + 2) continue;
        if (memcmp(buf, PROTO_DISC_REPLY, rlen) != 0) continue;

        size_t pos = rlen;
        u32 version = 0;
        for (int i = 0; i < 4; i++) version |= (u32)buf[pos + i] << (8 * i);
        pos += 4;
        if (version != PROTO_VERSION) continue;

        if (pos + 2 > (size_t)k) continue;
        u16 port = (u16)buf[pos] | ((u16)buf[pos + 1] << 8);
        pos += 2;

        host_t h;
        memset(&h, 0, sizeof(h));
        h.port = port ? port : PROTO_DEFAULT_PORT;
        inet_ntop(AF_INET, &from.sin_addr, h.ip, sizeof(h.ip));

        if (!take_str(buf, (size_t)k, &pos, h.name, sizeof(h.name)))
            snprintf(h.name, sizeof(h.name), "%s", h.ip);
        take_str(buf, (size_t)k, &pos, h.emu, sizeof(h.emu));

        if (!already_known(out, h.ip))
            out->v[out->n++] = h;
    }

    close(fd);
    return true;
}
