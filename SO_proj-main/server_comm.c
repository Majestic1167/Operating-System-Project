#include "server_comm.h"
#include <arpa/inet.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>

int start_server(int port, int backlog) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -2;
    }
    
    if (listen(fd, backlog) < 0) {
        close(fd);
        return -3;
    }
    
    return fd;
}

int accept_client(int server_fd) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    return accept(server_fd, (struct sockaddr*)&addr, &len);
}

ssize_t server_send(int sock, const void* buf, size_t len) {
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

ssize_t server_recv_all(int sock, void* buf, size_t len) {
    size_t rec = 0;
    char* p = buf;
    
    while (rec < len) {
        ssize_t n = read(sock, p + rec, len - rec);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1; // Connection closed
        rec += n;
    }
    return rec;
}

void close_connection(int sock) {
    if (sock >= 0) close(sock);
}