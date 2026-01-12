#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <ctype.h>
#include <pthread.h>
#include <signal.h>
#include "client_comm.h"
#include "sudoku_check.h"

// GLOBAL VARIABLES
volatile int tournament_time_left = -1;
pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;
char queued_message[1024] = {0};  // store hints or solver messages
int global_sock = -1; //for closing the client connection

typedef struct {
    char *board;
    int start_row;
    int start_col;
    pthread_mutex_t *lock;
    int *hints_used;
    int *changes_made; 
} SolverArgs;

void log_file_name(int id, char *buf, size_t sz) {
    snprintf(buf, sz, "client_%d_log.txt", id);
}

void write_log(int id, const char *msg) {
    char file[64];
    log_file_name(id, file, sizeof(file));
    FILE *f = fopen(file, "a");
    if (!f) return;
    
    time_t t = time(NULL);
    char *ts = ctime(&t);
    ts[strcspn(ts, "\n")] = 0;
    fprintf(f, "[CLIENT-%d] %s [%s]\n", id, msg, ts);
    fclose(f);
}

/* ---------- BRUTE FORCE BACKTRACKING (FALLBACK) ---------- */
int is_safe(char *board, int row, int col, char num) {
    for (int x = 0; x < 9; x++) {
        if (board[row * 9 + x] == num) return 0;
        if (board[x * 9 + col] == num) return 0;
    }
    int startRow = row - row % 3;
    int startCol = col - col % 3;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            if (board[(i + startRow) * 9 + (j + startCol)] == num) return 0;
        }
    }
    return 1;
}

int solve_sudoku_recursive(char *board) {
    int row = -1, col = -1;
    int isEmpty = 0;
    for (int i = 0; i < 81; i++) {
        if (board[i] == '0') {
            row = i / 9; col = i % 9; isEmpty = 1; break;
        }
    }
    if (!isEmpty) return 1;

    for (int num = 1; num <= 9; num++) {
        char c = num + '0';
        if (is_safe(board, row, col, c)) {
            board[row * 9 + col] = c;
            if (solve_sudoku_recursive(board)) return 1;
            board[row * 9 + col] = '0';
        }
    }
    return 0;
}

/* ---------- THREAD TEAM LOGIC ---------- */
void *solve_block_thread(void *arg) {
    SolverArgs *data = (SolverArgs*)arg;
    char *board = data->board;
    int start_r = data->start_row;
    int start_c = data->start_col;
    
    for (int r = start_r; r < start_r + 3; r++) {
        for (int c = start_c; c < start_c + 3; c++) {
            int idx = r * 9 + c;
            if (board[idx] != '0') continue;
            
            int possible[10] = {0};
            for (int k = 1; k <= 9; k++) possible[k] = 1;
            
            for (int k = 0; k < 9; k++) {
                if (board[r * 9 + k] != '0') possible[board[r * 9 + k] - '0'] = 0;
            }
            for (int k = 0; k < 9; k++) {
                if (board[k * 9 + c] != '0') possible[board[k * 9 + c] - '0'] = 0;
            }
            int br = (r / 3) * 3;
            int bc = (c / 3) * 3;
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) {
                    int val_idx = (br + i) * 9 + (bc + j);
                    if (board[val_idx] != '0') possible[board[val_idx] - '0'] = 0;
                }
            }
            
            int count = 0;
            int candidate = 0;
            for (int k = 1; k <= 9; k++) {
                if (possible[k]) {
                    count++; candidate = k;
                }
            }
            
            if (count == 1) {
                pthread_mutex_lock(data->lock);
                if (board[idx] == '0') {
                    board[idx] = candidate + '0';
                    (*data->changes_made)++;
                }
                pthread_mutex_unlock(data->lock);
            }
        }
    }
    return NULL;
}

void run_threaded_solver(char *board, int *hints_used) {
    pthread_mutex_t board_lock = PTHREAD_MUTEX_INITIALIZER;
    int iteration = 0;
    int changes_this_round = 0;
    int total_changes = 0;

    printf("\n[THREAD TEAM] Spawning 9 threads to analyze board...\n");

    do {
        iteration++;
        changes_this_round = 0;
        pthread_t threads[9];
        SolverArgs args[9];

        for(int i=0; i<9; i++) {
            args[i].board = board;
            args[i].start_row = (i / 3) * 3;
            args[i].start_col = (i % 3) * 3;
            args[i].lock = &board_lock;
            args[i].hints_used = hints_used;
            args[i].changes_made = &changes_this_round;
            pthread_create(&threads[i], NULL, solve_block_thread, &args[i]);
        }
        
        for(int i=0; i<9; i++) {
            pthread_join(threads[i], NULL);
        }
        
        if (changes_this_round > 0) {
            printf("[THREAD TEAM] Pass %d: Filled %d cells.\n", iteration, changes_this_round);
            total_changes += changes_this_round;
        }

    } while (changes_this_round > 0);

    int is_full = 1;
    for(int i=0; i<81; i++) if(board[i] == '0') is_full = 0;

    if (!is_full) {
        printf("[THREAD TEAM] Heuristics stuck. Activating Brute-Force Thread...\n");
        if (solve_sudoku_recursive(board)) {
            printf("[BRUTE FORCE] Puzzle completely solved!\n");
        } else {
            printf("[BRUTE FORCE] Failed. Puzzle might be unsolvable.\n");
        }
    } else {
        printf("[THREAD TEAM] Analysis complete. Board Solved.\n");
    }
}

/* ---------- DISPLAY & MAIN ---------- */
void display_board_with_messages(char *puzzle, char *solution) {
    pthread_mutex_lock(&print_lock);
    
    // Print Timer Status
    int t = 0;
    pthread_mutex_lock(&timer_lock);
    t = tournament_time_left;
    pthread_mutex_unlock(&timer_lock);
    
    if (t >= 0) {
        printf("\n⏳ Time left: %02d:%02d", t/60, t%60);
    } else {
        printf("\nMode: Single Player / Lobby");
    }

    printf("\n╔══════════════════════════════════════╗\n");
    printf("║              SUDOKU BOARD            ║\n");
    printf("╠══════════════════════════════════════╣\n");
    for (int i = 0; i < 9; i++) {
        if (i % 3 == 0 && i != 0) printf("║-------+-------+-------               ║\n");
        printf("║ ");
        for (int j = 0; j < 9; j++) {
            int idx = i * 9 + j;
            char c = (solution[idx] != '0') ? solution[idx] : (puzzle[idx] != '0') ? puzzle[idx] : '.';
            printf("%c ", c);
            if ((j + 1) % 3 == 0 && j != 8) printf("│ ");
        }
        printf("               ║\n");
    }
    printf("╚══════════════════════════════════════╝\n");

    if (queued_message[0] != '\0') {
        printf("%s\n", queued_message);
        queued_message[0] = '\0'; 
    }

    fflush(stdout);
    pthread_mutex_unlock(&print_lock);
}

// Returns 1 if correct (should exit), 0 if incorrect (should retry)
// Returns 1 if correct (should exit), 0 if incorrect (should retry)
int submit_solution(int sock, char *solution, int hints_used, int client_id, int tournament) {
    
    // Convert the char string 'solution' to a 9x9 int grid to check logic
    int grid[9][9];
    for (int i = 0; i < 81; i++) {
        if (solution[i] >= '0' && solution[i] <= '9') {
            grid[i / 9][i % 9] = solution[i] - '0';
        } else {
            grid[i / 9][i % 9] = 0; // Treat dots or bad chars as empty
        }
    }

    // Check for duplicates (invalid moves) locally
    if (!check_sudoku_partial(grid)) {
        pthread_mutex_lock(&print_lock);
        printf("\n\n [CLIENT CHECK] Invalid move detected (duplicates in row/col/box)!\n");
        printf("   Solution NOT sent to server. Please fix and retry.\n");
        pthread_mutex_unlock(&print_lock);
        return 0; // Return 0 to let the user retry without disconnecting
    }
   

    if (client_send(sock, solution, 81) < 0) {
        write_log(client_id, "Failed to send solution");
        return -1;
    }
    char hints_msg[10];
    snprintf(hints_msg, sizeof(hints_msg), "%d", hints_used);
    if (send(sock, hints_msg, strlen(hints_msg) + 1, 0) < 0) {
        write_log(client_id, "Failed to send hints count");
        return -1;
    }
    
    write_log(client_id, "Solution sent");
    printf("Solution sent (hints: %d/2). Waiting...\n", hints_used);
    
    char result[256] = {0};
    size_t idx = 0;
    while (idx < sizeof(result) - 1) {
        ssize_t n = recv(sock, result + idx, 1, 0);
        if (n <= 0) break;
        if (result[idx] == '\0') break;
        idx++;
    }
    
    if (idx > 0) {
        pthread_mutex_lock(&print_lock);
        printf("\n╔══════════════════════════════════════╗\n");
        printf("║           SERVER RESPONSE            ║\n");
        printf("╠══════════════════════════════════════╣\n");
        if (tournament || strstr(result, "TOURNAMENT")) 
            printf("║        🏆 TOURNAMENT RESULT 🏆       ║\n");
        printf("║ %-36s ║\n", result);
        printf("╚══════════════════════════════════════╝\n");
        pthread_mutex_unlock(&print_lock);
        
        write_log(client_id, result);
        if (strstr(result, "PERFECT") || strstr(result, "EXCELLENT") || 
            strstr(result, "GOOD! Solved") || strstr(result, "TOURNAMENT")) {
            return 1; // Correct! Exit loop
        }
    }
    return 0; // Incorrect, retry
}

int handle_tournament(int sock, int client_id) {
    char buffer[256];
    int n = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) return 0;
    
    buffer[n] = '\0';
    printf("%sYour choice (y/n): ", buffer);
    
    char choice[10];
    if (fgets(choice, sizeof(choice), stdin) == NULL) return 0;
    
    if (send(sock, &choice[0], 1, 0) < 0) return 0;
    
    if (choice[0] == 'y' || choice[0] == 'Y') {
        write_log(client_id, "Tournament accepted");
        while (1) {
            n = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (n <= 0) return 0;
            buffer[n] = '\0';
            if (strncmp(buffer, "START", 5) == 0) {
                int seconds = 0;
                sscanf(buffer, "START %d", &seconds);

                pthread_mutex_lock(&timer_lock);
                tournament_time_left = seconds;
                pthread_mutex_unlock(&timer_lock);

                printf("\n🏁 Tournament started! Time: %d seconds\n", seconds);
                return 1;
            } else if (strstr(buffer, "Waiting") || strstr(buffer, "full")) {
                printf("%s", buffer);
            } else {
                printf("%s", buffer);
            }
        }
    } else {
        recv(sock, buffer, sizeof(buffer)-1, 0);
    }
    return 0;
}

void* timeup_listener(void* arg) {
    int sock = *(int*)arg;
    char buf[10];
    while (recv(sock, buf, sizeof(buf) - 1, MSG_PEEK) > 0) {
        if (strstr(buf, "TIMEUP")) {
            pthread_mutex_lock(&print_lock);
            printf("\n\n⏰ [TIME EXPIRED] Tournament ended! Exiting in 3s...\n");
            pthread_mutex_unlock(&print_lock);
            sleep(3);
            exit(0); // FORCE EXIT
        }
        sleep(1);
    }
    return NULL;
}

void* tournament_timer_thread(void* arg) {
    (void)arg;
    while (1) {
        sleep(1);
        
        // 1. Lock and check time
        pthread_mutex_lock(&timer_lock);
        if (tournament_time_left <= 0) {
            pthread_mutex_unlock(&timer_lock);
            if (tournament_time_left == 0) break; // Time expired, exit thread
            continue; // Timer hasn't started yet
        }
        
        // 2. Decrement time WITHOUT creating an unused variable
        --tournament_time_left; 
        
        pthread_mutex_unlock(&timer_lock);
    }
    return NULL;
}

void give_hint(char *solution, int *hints_used) {
    if (*hints_used >= 2) {
        pthread_mutex_lock(&print_lock);
        strcpy(queued_message, "No more hints available!");
        pthread_mutex_unlock(&print_lock);
        return;
    }

    int empty[81], empty_count = 0;
    for (int i = 0; i < 81; i++) if (solution[i] == '0') empty[empty_count++] = i;
    if (empty_count == 0) {
        pthread_mutex_lock(&print_lock);
        strcpy(queued_message, "No empty cells!");
        pthread_mutex_unlock(&print_lock);
        return;
    }

    int hints_to_show = (*hints_used == 0) ? 2 : 1;
    hints_to_show = (hints_to_show > empty_count) ? empty_count : hints_to_show;

    char msg[256] = {0};
    snprintf(msg, sizeof(msg), "Hint (%d/2):", *hints_used + 1);
    for (int h = 0; h < hints_to_show; h++) {
        int pos = empty[rand() % empty_count];
        char cell_msg[64];
        snprintf(cell_msg, sizeof(cell_msg), "  Focus on cell (%d,%d)", pos/9 + 1, pos%9 + 1);
        strncat(msg, "\n", sizeof(msg) - strlen(msg) - 1);
        strncat(msg, cell_msg, sizeof(msg) - strlen(msg) - 1);
    }

    pthread_mutex_lock(&print_lock);
    strcpy(queued_message, msg);
    pthread_mutex_unlock(&print_lock);

    (*hints_used)++;
}

void interactive_menu(char *puzzle, char *solution, int *hints_used, int sock, int client_id, int tournament) {
    strcpy(solution, puzzle);
    pthread_t tid;
    if (tournament) {
        pthread_create(&tid, NULL, timeup_listener, &sock);
        pthread_detach(tid);
    }

    pthread_t timer_tid;
    if (tournament) {
        pthread_create(&timer_tid, NULL, tournament_timer_thread, NULL);
        pthread_detach(timer_tid);
    }
    
    while (1) {
        char peek_buf[10] = {0};
        if (recv(sock, peek_buf, sizeof(peek_buf) - 1, MSG_DONTWAIT | MSG_PEEK) > 0) {
            if (strstr(peek_buf, "TIMEUP")) {
                recv(sock, peek_buf, sizeof(peek_buf), MSG_DONTWAIT); 
                printf("\n\n⏰ [TOURNAMENT] Exiting...\n"); sleep(2); exit(0);
            }
        }

        display_board_with_messages(puzzle, solution);
        
        if (tournament) printf("\n⏱️  TOURNAMENT: Enter choice quickly! (1-9): ");
        else printf("\nEnter choice (1-9): ");
        
        printf("\n1. Enter complete solution");
        printf("\n2. Enter partial solution");
        printf("\n3. Get hint (%d/2 used)", *hints_used);
        printf("\n4. Submit current solution");
        printf("\n5. Show current board");
        printf("\n6. Exit and Submit");
        printf("\n7. Exit without Submitting");
        printf("\n8. Auto-Solve (Multi-threaded Team)");
        printf("\n9. Reset Board\n> ");
        
        char input[256];
        if (fgets(input, sizeof(input), stdin) == NULL) break;
        input[strcspn(input, "\n")] = 0;
        if (strlen(input) == 0) continue;

        int choice = atoi(input);
        if (choice == 1) {
            printf("\nEnter 81 digits: ");
            char complete[256];
            if (fgets(complete, sizeof(complete), stdin) == NULL) continue;
            complete[strcspn(complete, "\n")] = 0;
            int pos = 0;
            for (int i = 0; complete[i] != '\0' && pos < 81; i++) {
                if (isdigit(complete[i])) solution[pos++] = complete[i];
            }
            printf("Solution updated.\n");
            
        } else if (choice == 2) {
            printf("\na) Complete 81 digits (0s for empty)\nb) Row by row\nc) Position and value\nChoice: ");
            char method[10];
            fgets(method, sizeof(method), stdin);
            if (method[0] == 'a') {
                printf("Enter 81 digits: ");
                char partial[256];
                fgets(partial, sizeof(partial), stdin);
                int pos = 0;
                for (int i = 0; partial[i] != '\0' && pos < 81; i++) 
                    if (isdigit(partial[i])) solution[pos++] = partial[i];
                printf("Board updated.\n");
            } else if (method[0] == 'b') {
                printf("Enter 9 digits for each row (0 for empty):\n");
                for (int r = 0; r < 9; r++) {
                    printf("Row %d: ", r + 1);
                    char rowbuf[64];
                    fgets(rowbuf, sizeof(rowbuf), stdin);
                    int c = 0;
                    for (int k = 0; rowbuf[k] && c < 9; k++) {
                        if (isdigit(rowbuf[k])) {
                            solution[r * 9 + c] = rowbuf[k];
                            c++;
                        }
                    }
                }
                printf("Board updated row by row.\n");
            } else if (method[0] == 'c') {
                printf("Enter 'position value' (e.g. 1 5). 0 0 to finish.\n");
                while (1) {
                    printf("> ");
                    char line[64];
                    fgets(line, sizeof(line), stdin);
                    int pos, val;
                    if (sscanf(line, "%d %d", &pos, &val) == 2) {
                        if (pos == 0 && val == 0) break;
                        if (pos >= 1 && pos <= 81 && val >= 0 && val <= 9) {
                            solution[pos-1] = val + '0';
                            printf("Set pos %d to %d\n", pos, val);
                        }
                    }
                }
                printf("Board updated.\n");
            }
            
        } else if (choice == 3) {
            give_hint(solution, hints_used);
            
        } else if (choice == 4) {
            if (submit_solution(sock, solution, *hints_used, client_id, tournament) == 1) {
                printf("\nPuzzle solved!\n"); return;
            }
            
        } else if (choice == 5) {
            continue;
            
        } else if (choice == 6) {
            submit_solution(sock, solution, *hints_used, client_id, tournament);
            printf("Goodbye!\n"); return;
            
        } else if (choice == 7) {
            printf("\nExiting...\n"); return;
            
        } else if (choice == 8) {
            run_threaded_solver(solution, hints_used);
            
        } else if (choice == 9) {
            strcpy(solution, puzzle);
            printf("Board reset to initial state.\n");
            
        } else {
            printf("Invalid choice!\n");
        }
    }
}

void handle_sigint(int sig) {
    (void)sig; // Suppress unused parameter warning
    printf("\n\n[CLIENT] Caught Signal (Ctrl+C). Exiting cleanly...\n");
    if (global_sock != -1) {
        close(global_sock);
    }
    exit(0);
}

/* ---------- Main (Updated for Config File) ---------- */
int main(int argc, char *argv[]) {
    // 1. Setup Config File Logic
    char *config_file = "client_config.txt";

    // Allow user to pass a different file: ./client my_other_config.txt
    if (argc == 2) {
        config_file = argv[1];
    }

    // 2. Open and Read the Config File
    FILE *f = fopen(config_file, "r");
    if (!f) {
        fprintf(stderr, " Error: Could not open configuration file '%s'\n", config_file);
        fprintf(stderr, "   Make sure 'client_config.txt' exists with IP and Port.\n");
        return 1;
    }

    char ip[64];
    int port;

    // Expect format: 127.0.0.1 8082
    if (fscanf(f, "%63s %d", ip, &port) != 2) {
        fprintf(stderr, " Error: Invalid format in '%s'. Expected: <IP> <PORT>\n", config_file);
        fclose(f);
        return 1;
    }
    fclose(f);

    // 3. Signal Handling
    signal(SIGPIPE, SIG_IGN);

    signal(SIGINT, handle_sigint); 

    srand(time(NULL));
    
    printf("Loading config from %s...\n", config_file);
    printf(" Connecting to Server at %s:%d...\n", ip, port);

    // 4. Connect using the data from the file
    int sock = connect_to_server(ip, port);
    if (sock < 0) {
        fprintf(stderr, " Failed to connect to server. Is it running on Port %d?\n", port);
        return 1;
    }
    
    // 5. Rest of the standard logic (Receive ID, Game Loop)
    int my_id;
    if (client_recv_all(sock, &my_id, sizeof(my_id)) != sizeof(my_id)) {
        close_connection(sock); return 1;
    }
    
    char logf[64];
    log_file_name(my_id, logf, sizeof(logf));
    FILE *clr = fopen(logf, "w");
    if (clr) fclose(clr);
    
    write_log(my_id, "Connected");
    printf(" Connected as Client %d\n", my_id);
    
    char puzzle[82];
    if (client_recv_all(sock, puzzle, 81) != 81) {
        close_connection(sock); return 1;
    }
    puzzle[81] = '\0';
    
    printf("\n╔══════════════════════════════════════╗\n");
    printf("║     WELCOME TO SUDOKU SOLVER!        ║\n");
    printf("╠══════════════════════════════════════╣\n");
    printf("║ Client ID: %d                         ║\n", my_id);
    printf("╚══════════════════════════════════════╝\n");
    
    int tournament = handle_tournament(sock, my_id);
    if (tournament) {
        printf("\n════════════════════════════════════════\n");
        printf("         🏆 TOURNAMENT MODE 🏆         \n");
        printf("════════════════════════════════════════\n");
    }
    
    char solution[82];
    int hints_used = 0;
    interactive_menu(puzzle, solution, &hints_used, sock, my_id, tournament);
    
    close_connection(sock);
    write_log(my_id, "Disconnected");
    return 0;
}