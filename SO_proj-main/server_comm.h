#ifndef SERVER_COMM_H
#define SERVER_COMM_H

#include <unistd.h>
#include <sys/types.h>

int start_server(int port, int backlog);
int accept_client(int server_fd);
ssize_t server_send(int sock, const void *buf, size_t len);
ssize_t server_recv_all(int sock, void *buf, size_t len);
void close_connection(int sock);

#endif