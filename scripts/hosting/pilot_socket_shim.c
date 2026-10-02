/* Test-only Linux bind shim for the unchanged pilot server. Never ship this.
 * The launcher prepares one unbound IPv4 UDP socket and drops all privileges.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

int bind(int descriptor, const struct sockaddr *address, socklen_t length) {
    int (*real_bind)(int, const struct sockaddr *, socklen_t) = dlsym(RTLD_NEXT, "bind");
    if (!real_bind) {
        errno = ENOSYS;
        return -1;
    }
    const char *configured = getenv("PILOT_PREPARED_UDP_FD");
    if (configured && address && address->sa_family == AF_INET &&
        length >= sizeof(struct sockaddr_in) &&
        ntohs(((const struct sockaddr_in *) address)->sin_port) == 64738) {
        int type = 0;
        socklen_t size = sizeof(type);
        if (getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &type, &size) == 0 && type == SOCK_DGRAM) {
            char *end = NULL;
            long source = strtol(configured, &end, 10);
            if (!end || *end || source < 3 || source > 1024 || (int) source == descriptor) {
                errno = EINVAL;
                return -1;
            }
            /* Keep the option the server sets before bind; change only RCVBUF. */
            int packet_info = 0;
            size = sizeof(packet_info);
            if (getsockopt(descriptor, IPPROTO_IP, IP_PKTINFO, &packet_info, &size) != 0 ||
                setsockopt((int) source, IPPROTO_IP, IP_PKTINFO, &packet_info, size) != 0) {
                return -1;
            }
            if (dup2((int) source, descriptor) < 0) {
                return -1;
            }
            close((int) source);
            unsetenv("PILOT_PREPARED_UDP_FD");
            int actual = 0;
            size = sizeof(actual);
            if (getsockopt(descriptor, SOL_SOCKET, SO_RCVBUF, &actual, &size) != 0) {
                return -1;
            }
            FILE *record = fopen("/runtime/socket-bound.json", "wx");
            if (!record) {
                return -1;
            }
            fprintf(record, "{\"effective_receive_buffer_bytes\":%d,\"uid\":%u,\"ip_pktinfo\":%d}\n",
                    actual, (unsigned) getuid(), packet_info);
            fclose(record);
        }
    }
    return real_bind(descriptor, address, length);
}
