#include "client_comm.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>

int connect_to_server(const char *ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        close(sock);
        return -2;
    }
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -3;
    }
    
    return sock;
}

ssize_t client_send(int sock, const void* buf, size_t len) {
    const char* p = buf;
    size_t rem = len;
    
    while (rem > 0) {
        ssize_t n = send(sock, p, rem, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        p += n;
        rem -= n;
    }
    return len;
}

ssize_t client_recv_all(int sock, void* buf, size_t len) {
    size_t rec = 0;
    char* p = buf;
    
    while (rec < len) {
        ssize_t n = read(sock, p + rec, len - rec);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1; 
        rec += n;
    }
    return rec;
}

void close_connection(int sock) {
    if (sock >= 0) close(sock);
}