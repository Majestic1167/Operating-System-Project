CC = gcc
CFLAGS = -Wall -Wextra -pthread -D_XOPEN_SOURCE=700
SERVER = server
CLIENT = client

all: $(SERVER) $(CLIENT)

$(SERVER): server.c sudoku_check.c server_comm.c
	$(CC) $(CFLAGS) server.c sudoku_check.c server_comm.c -o $(SERVER)

$(CLIENT): client.c client_comm.c sudoku_check.c
	$(CC) $(CFLAGS) client.c client_comm.c sudoku_check.c -o $(CLIENT)

clean:
	rm -f $(SERVER) $(CLIENT) client_*_log.txt server_log.txt
	

run_server: $(SERVER)
	./$(SERVER)

run_client: $(CLIENT)
	./$(CLIENT) 

.PHONY: all clean run_server run_client