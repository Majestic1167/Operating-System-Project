#ifndef CLIENT_COMM_H
#define CLIENT_COMM_H

#include <unistd.h>
#include <sys/types.h>

int connect_to_server(const char *ip, int port);
ssize_t client_send(int sock, const void *buf, size_t len);
ssize_t client_recv_all(int sock, void *buf, size_t len);
void close_connection(int sock);

#endif