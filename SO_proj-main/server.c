#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/socket.h>
#include <errno.h>

#include "sudoku_check.h"
#include "server_comm.h"

#define PORT 8082
#define MAX_GAMES 64
#define MAX_ACTIVE_CLIENTS 3
#define TOURNAMENT_SIZE 2
#define TOURNAMENT_TIME 120
#define QUEUE_CAPACITY 20
#define LEADERBOARD_SIZE 10

/* ---------- Structures ---------- */
struct Game {
    char id[10];
    char puzzle[82];
    char solution[82];
};

typedef struct {
    int client_id;
    char puzzle[82];
    char solution[82];
    time_t connect_time;
    time_t puzzle_sent_time;
    time_t solution_received_time;
    double puzzle_solve_time;
    int hints_used;
    int solved_correctly;
    pthread_mutex_t stat_lock;
} ClientStats;

typedef struct QueueNode {
    int socket;
    int client_id;
    struct Game game;
    time_t arrival_time;
    struct QueueNode* next;
} QueueNode;

typedef struct {
    QueueNode* front;
    QueueNode* rear;
    int size;
    int capacity;
    pthread_mutex_t queue_lock;
    sem_t slots_available; 
    sem_t items_available; 
} RequestQueue;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;       
    pthread_cond_t start_cond; 
    pthread_cond_t turn_cond;  
    int participants;
    int required_participants;
    int tournament_active;
    int tournament_started;
    int tournament_finished;
    time_t start_time;
    int duration;
    int client_socks[TOURNAMENT_SIZE];
    int session_id;
} Tournament;

typedef struct {
    int client_id;
    int solved_correctly;
    double solve_time;   
} LeaderboardEntry;

typedef struct {
    LeaderboardEntry entries[LEADERBOARD_SIZE];
    int count;
    pthread_mutex_t lock;
} Leaderboard;

typedef struct {
    double avgTime;
    double bestTime;
    double longestMatch;
    double shortestMatch;
    int correctSols;
    pthread_mutex_t lock;
} TournamentStatistics;

typedef struct {
    time_t time;
    pthread_mutex_t lock;
} TimeStore;

/* ---------- Globals ---------- */
volatile sig_atomic_t keep_running = 1;
int server_fd_global = -1;
int total_games = 0;
struct Game games[MAX_GAMES];
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
sem_t active_clients_sem;
RequestQueue client_queue;
Tournament tournament;
TournamentStatistics tStats;
TimeStore tStore;

// Statistics
int connected_clients = 0;
int games_in_progress = 0; 
int waiting_for_slot = 0; 
int solved_games = 0;
int abandoned_games = 0;
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

int client_counter = 0;
Leaderboard tournament_leaderboard;

/* ---------- Helper Functions ---------- */
void write_log(const char *id, const char *msg) {
    pthread_mutex_lock(&log_mutex);
    FILE *f = fopen("server_log.txt", "a");
    if (f) {
        time_t t = time(NULL);
        char *ts = ctime(&t);
        ts[strcspn(ts, "\n")] = 0;
        fprintf(f, "%s %s [%s]\n", id, msg, ts);
        fclose(f);
    }
    pthread_mutex_unlock(&log_mutex);
}

void handle_sigint(int sig) {
    (void)sig;
    keep_running = 0;
    
    if (server_fd_global >= 0) {
        shutdown(server_fd_global, SHUT_RDWR);
        close(server_fd_global);
        server_fd_global = -1;
    }

    sem_post(&client_queue.items_available);
    sem_post(&active_clients_sem);

    pthread_mutex_lock(&tournament.lock);
    pthread_cond_broadcast(&tournament.cond);
    pthread_cond_broadcast(&tournament.start_cond);
    pthread_cond_broadcast(&tournament.turn_cond);
    pthread_mutex_unlock(&tournament.lock);

    printf("\nReceived SIGINT. Shutting down...\n");
    write_log("[SERVER]", "Received SIGINT. Initiating shutdown.");
}
/*TournamentStatistics functions*/
void init_t_stats(TournamentStatistics* ts){
    ts->correctSols = 0;
    ts->avgTime = 0;
    ts->bestTime = 0;
    ts->shortestMatch = 0;
    ts->longestMatch = 0;

}
void update_t_stats(TournamentStatistics* ts, double entry){
   
    pthread_mutex_lock(&ts->lock);
    printf("Received %f \n",entry);
    ts->correctSols += 1;

    if(ts->avgTime == 0){
        ts->avgTime = entry;
    }
    else{
        ts->avgTime += (entry - ts->avgTime) / ts->correctSols;
    }

    if(ts->bestTime == 0){
        ts->bestTime = entry;
    }
    else{
        if(ts->bestTime > entry){
            ts->bestTime = entry;
        }
    }
    pthread_mutex_unlock(&ts->lock);

}

void update_t_stats_match(TournamentStatistics* ts, double entry){

    if(ts->shortestMatch == 0){
        ts->shortestMatch = entry;
    }
    else{
        if(ts->shortestMatch > entry){
            ts->shortestMatch = entry;
        }
    }

    if(ts->longestMatch == 0){
        ts->longestMatch = entry;
    }
    else{
        if(ts->longestMatch < entry){
            ts->longestMatch = entry;
        }
    }
}
void show_t_stats(TournamentStatistics* ts){
    printf("Correct solutions: %d \n", ts->correctSols);
    printf("Best time: %8.2f \n", ts->bestTime);
    printf("Average time: %8.2f \n", ts->avgTime);

}
void show_t_stats_match(TournamentStatistics* ts){
    printf("----------------------------------------\n");
    printf("Shortest match: %8.2f \n", ts->shortestMatch);
    printf("Longest match: %8.2f \n", ts->longestMatch);
    printf("----------------------------------------\n");

}
/*------------TimeStore functions---------*/
void set_tStore_timeStamp(TimeStore* tSt){
    pthread_mutex_lock(&tSt->lock);
    tSt->time = time(NULL);
    pthread_mutex_unlock(&tSt->lock);
}
time_t get_tStore_timeStamp(TimeStore* tSt){
    pthread_mutex_lock(&tSt->lock);
    time_t time;
    time = tSt->time;
    pthread_mutex_unlock(&tSt->lock);
    return time;
}
/* ---------- Tournament Logic ---------- */
void init_tournament(Tournament* t, int required) {
    pthread_mutex_init(&t->lock, NULL);
    pthread_cond_init(&t->cond, NULL);
    pthread_cond_init(&t->start_cond, NULL);
    pthread_cond_init(&t->turn_cond, NULL);
    t->participants = 0;
    t->required_participants = required;
    t->tournament_active = 0;
    t->tournament_started = 0;
    t->tournament_finished = 0; // Initialize
}

void* tournament_timer_thread(void* arg) {
    Tournament* t = (Tournament*)arg;
    
    pthread_mutex_lock(&t->lock);
    int current_session = t->session_id;
    pthread_mutex_unlock(&t->lock);
    
    for (int i = 0; i < TOURNAMENT_TIME && keep_running; i++) {
        sleep(1);
        pthread_mutex_lock(&t->lock);
        if (t->session_id != current_session || !t->tournament_active) {
            pthread_mutex_unlock(&t->lock);
            return NULL; 
        }
        pthread_mutex_unlock(&t->lock);
    }

    pthread_mutex_lock(&t->lock);
    if (t->tournament_active && t->session_id == current_session) {
        printf("[TOURNAMENT] Time expired for session %d!\n", current_session);
        for (int i = 0; i < TOURNAMENT_SIZE; i++) {
            if (t->client_socks[i] >= 0) {
                send(t->client_socks[i], "TIMEUP", 7, 0);
                shutdown(t->client_socks[i], SHUT_RDWR);
            }
        }
        t->tournament_active = 0;
        t->tournament_started = 0;
        t->tournament_finished = 1;
    }
    pthread_mutex_unlock(&t->lock);
    return NULL;
}


void join_tournament(Tournament* t, int client_id, int sock) {
    pthread_mutex_lock(&t->lock);
    char log_buf[256];
    char cid_str[32];
    snprintf(cid_str, sizeof(cid_str), "[CLIENT-%d]", client_id);
    
    while ((t->tournament_active || t->tournament_finished) && keep_running) {
        char wait_msg[256];
        snprintf(wait_msg, sizeof(wait_msg), "Tournament is busy. Waiting for reset (Active: %d, Finished: %d)...\n", 
                 t->tournament_active, t->tournament_finished);
        server_send(sock, wait_msg, strlen(wait_msg) + 1);
        
        snprintf(log_buf, sizeof(log_buf), "Queued for Tournament (Active: %d, Fin: %d)", t->tournament_active, t->tournament_finished);
        write_log(cid_str, log_buf);
        
        printf("Client %d waiting for tournament to finish...\n", client_id);
        pthread_cond_wait(&t->turn_cond, &t->lock);
    }

    if (!keep_running) {
        pthread_mutex_unlock(&t->lock);
        return;
    }

    t->participants++;
    int current_count = t->participants;
    int ready = (current_count >= t->required_participants);
    
    snprintf(log_buf, sizeof(log_buf), "Joined Tournament Lobby (%d/%d)", current_count, t->required_participants);
    write_log(cid_str, log_buf);
    
    printf("Client %d joined tournament (%d/%d)\n", client_id, current_count, t->required_participants);
    
    t->client_socks[t->participants - 1] = sock;
    if (ready && !t->tournament_active) {
        t->session_id++;
        t->tournament_active = 1;
        t->tournament_started = 1;
        t->tournament_finished = 0; // Ensure reset
        pthread_t timer_tid;
        pthread_create(&timer_tid, NULL, tournament_timer_thread, t);
        pthread_detach(timer_tid);
        pthread_cond_broadcast(&t->cond);
        t->start_time = time(NULL);
        t->duration = TOURNAMENT_TIME;
        pthread_cond_broadcast(&t->cond);
        pthread_cond_broadcast(&t->start_cond);
        
        write_log(cid_str, "Tournament capacity met. Broadcasting START.");
        printf("Tournament #%d starting! (%d players)\n", t->session_id, current_count);
        set_tStore_timeStamp(&tStore);
    } else {
        char waiting_msg[] = "Waiting for opponents...\n";
        server_send(sock, waiting_msg, strlen(waiting_msg) + 1);
        write_log(cid_str, "Waiting in lobby for opponent.");
        
        while (!t->tournament_active && keep_running) {
    pthread_cond_wait(&t->cond, &t->lock);
}
if (!keep_running) {
    pthread_mutex_unlock(&t->lock);
    return;
}

        while (!t->tournament_started && keep_running) {
            pthread_cond_wait(&t->start_cond, &t->lock);
        }
    }
    pthread_mutex_unlock(&t->lock);
}

void leave_tournament(Tournament* t) {
    pthread_mutex_lock(&t->lock);
    
    // Only mark finished if game was actually active
    if (t->tournament_active) {
        t->tournament_finished = 1; 
        t->tournament_active = 0;
        t->tournament_started = 0;
    }

    if (t->participants > 0) {
        t->participants--;
        printf("Player left tournament. Remaining: %d\n", t->participants);
        
        if (t->participants == 0) {
            for (int i = 0; i < TOURNAMENT_SIZE; i++) {
    t->client_socks[i] = -1;
}
            time_t time1;
            double diff;
            t->tournament_finished = 0; // Fully reset, open for new players
            printf("Tournament empty. Resetting for next group.\n");
            write_log("[TOURNAMENT]", "Tournament finished. Resetting state.");
            time1 = get_tStore_timeStamp(&tStore);
            diff = difftime(time(NULL),time1);
            update_t_stats_match(&tStats,diff);
            show_t_stats_match(&tStats);
            pthread_cond_broadcast(&t->turn_cond); // Wake up waiters (Client 3/4)
        }
    }
    pthread_mutex_unlock(&t->lock);
}

void destroy_tournament(Tournament* t) {
    pthread_mutex_destroy(&t->lock);
    pthread_cond_destroy(&t->cond);
    pthread_cond_destroy(&t->start_cond);
    pthread_cond_destroy(&t->turn_cond);
}

/* ---------- Leaderboard -----------*/
void add_to_leaderboard(int client_id, int solved, double solve_time) {
    pthread_mutex_lock(&tournament_leaderboard.lock);
    
    // Create new entry
    LeaderboardEntry new_entry;
    new_entry.client_id = client_id;
    new_entry.solved_correctly = solved;
    new_entry.solve_time = solve_time;
    update_t_stats(&tStats,solve_time);
    
    // Find position to insert (keep sorted by time)
    int insert_pos = tournament_leaderboard.count;
    
    // If leaderboard is full, check if new entry is better than worst
    if (tournament_leaderboard.count >= LEADERBOARD_SIZE) {
        // Find worst entry (highest time among correct solutions)
        double worst_time = -1;
        int worst_pos = -1;
        for (int i = 0; i < LEADERBOARD_SIZE; i++) {
            if (tournament_leaderboard.entries[i].solved_correctly == 0) {
                worst_pos = i;
                break;
            } else if (tournament_leaderboard.entries[i].solve_time > worst_time) {
                worst_time = tournament_leaderboard.entries[i].solve_time;
                worst_pos = i;
            }
        }
        
        // If new entry is worse than worst, don't insert
        if (worst_pos >= 0) {
            if (new_entry.solved_correctly == 0 || 
                (new_entry.solved_correctly == 1 && new_entry.solve_time >= worst_time)) {
                pthread_mutex_unlock(&tournament_leaderboard.lock);
                return;
            }
            // Replace worst entry
            tournament_leaderboard.entries[worst_pos] = new_entry;
            insert_pos = worst_pos;
        } else {
            pthread_mutex_unlock(&tournament_leaderboard.lock);
            return;
        }
    } else {
        // Not full, add at end
        tournament_leaderboard.entries[tournament_leaderboard.count] = new_entry;
        tournament_leaderboard.count++;
    }
    
    // Sort after insertion (or bubble up)
    for (int i = insert_pos; i > 0; i--) {
        LeaderboardEntry* curr = &tournament_leaderboard.entries[i];
        LeaderboardEntry* prev = &tournament_leaderboard.entries[i-1];
        
        if (curr->solved_correctly > prev->solved_correctly ||
            (curr->solved_correctly == prev->solved_correctly && 
             curr->solved_correctly == 1 && curr->solve_time < prev->solve_time)) {
            // Swap
            LeaderboardEntry temp = *curr;
            *curr = *prev;
            *prev = temp;
        } else {
            break;
        }
    }
    
    pthread_mutex_unlock(&tournament_leaderboard.lock);
}

int compare_leaderboard_entries(const void* a, const void* b) {
    LeaderboardEntry* entry_a = (LeaderboardEntry*)a;
    LeaderboardEntry* entry_b = (LeaderboardEntry*)b;
    
    // First, prioritize correct solutions over incorrect ones
    if (entry_a->solved_correctly != entry_b->solved_correctly) {
        return entry_b->solved_correctly - entry_a->solved_correctly;
    }
    
    // If both have correct solutions, sort by solve time (ascending)
    if (entry_a->solved_correctly == 1 && entry_b->solved_correctly == 1) {
        if (entry_a->solve_time < entry_b->solve_time) return -1;
        if (entry_a->solve_time > entry_b->solve_time) return 1;
        return 0;
    }
    
    // If both incorrect, keep original order
    return 0;
}


void print_leaderboard() {
    pthread_mutex_lock(&tournament_leaderboard.lock);
    
    // Create a copy for sorting
    LeaderboardEntry temp[LEADERBOARD_SIZE];
    int count = tournament_leaderboard.count;
    
    for (int i = 0; i < count; i++) {
        temp[i] = tournament_leaderboard.entries[i];
    }
    
    // Sort using qsort
    qsort(temp, count, sizeof(LeaderboardEntry), compare_leaderboard_entries);
    
    printf("\n🏆 GLOBAL TOURNAMENT LEADERBOARD 🏆\n");
    printf("Rank | Client | Correct | Solve Time (s)\n");
    printf("----------------------------------------\n");
    
    for (int i = 0; i < count; i++) {
        printf("%4d | %6d |    %d    | %8.2f\n",
               i + 1,
               temp[i].client_id,
               temp[i].solved_correctly,
               temp[i].solve_time);

    }
    
    printf("----------------------------------------\n");
    show_t_stats(&tStats);
    printf("----------------------------------------\n");
    pthread_mutex_unlock(&tournament_leaderboard.lock);
}

/* ---------- Queue Logic ---------- */
void init_queue(RequestQueue* q, int capacity) {
    q->front = q->rear = NULL;
    q->size = 0;
    q->capacity = capacity;
    pthread_mutex_init(&q->queue_lock, NULL);
    sem_init(&q->slots_available, 0, capacity);
    sem_init(&q->items_available, 0, 0);
}

int enqueue(RequestQueue* q, int socket, int client_id, struct Game game) {
    if (sem_trywait(&q->slots_available) != 0) return -1;
    
    QueueNode* new_node = malloc(sizeof(QueueNode));
    if (!new_node) {
        sem_post(&q->slots_available);
        return -1;
    }
    
    new_node->socket = socket;
    new_node->client_id = client_id;
    new_node->game = game;
    new_node->arrival_time = time(NULL);
    new_node->next = NULL;
    
    pthread_mutex_lock(&q->queue_lock);
    if (q->rear == NULL) q->front = q->rear = new_node;
    else {
        q->rear->next = new_node;
        q->rear = new_node;
    }
    q->size++;
    pthread_mutex_unlock(&q->queue_lock);
    
    sem_post(&q->items_available);
    return 0;
}

QueueNode* dequeue(RequestQueue* q) {
    sem_wait(&q->items_available);
    if (!keep_running) return NULL;

    pthread_mutex_lock(&q->queue_lock);
    if (q->front == NULL) {
        pthread_mutex_unlock(&q->queue_lock);
        return NULL;
    }
    
    QueueNode* node = q->front;
    q->front = q->front->next;
    if (q->front == NULL) q->rear = NULL;
    q->size--;
    pthread_mutex_unlock(&q->queue_lock);
    
    sem_post(&q->slots_available);
    return node;
}

void display_queue_status(RequestQueue* q) {
    pthread_mutex_lock(&q->queue_lock);
    int real_waiting = q->size + waiting_for_slot;
    printf("[STATUS] Queue: %d/%d | Active Clients: %d/%d | Tourn lobby: %d/%d\n", 
           real_waiting, q->capacity, games_in_progress, MAX_ACTIVE_CLIENTS,
           tournament.participants, TOURNAMENT_SIZE);
    pthread_mutex_unlock(&q->queue_lock);
}

/* ---------- Statistics ---------- */
ClientStats* create_client_stats(int client_id, const char* puzzle) {
    ClientStats* stats = malloc(sizeof(ClientStats));
    stats->client_id = client_id;
    strcpy(stats->puzzle, puzzle);
    stats->connect_time = time(NULL);
    stats->puzzle_sent_time = 0;
    stats->solution_received_time = 0;
    stats->puzzle_solve_time = 0.0;
    stats->hints_used = 0;
    stats->solved_correctly = 0;
    pthread_mutex_init(&stats->stat_lock, NULL);
    return stats;
}

void update_stats_puzzle_sent(ClientStats* stats) {
    pthread_mutex_lock(&stats->stat_lock);
    stats->puzzle_sent_time = time(NULL);
    pthread_mutex_unlock(&stats->stat_lock);
}

void update_stats_solution_received(ClientStats* stats, int hints_used, int solved_correctly) {
    pthread_mutex_lock(&stats->stat_lock);
    stats->solution_received_time = time(NULL);
    stats->hints_used = hints_used;
    stats->solved_correctly = solved_correctly;
    if (stats->puzzle_sent_time > 0) {
        stats->puzzle_solve_time = difftime(stats->solution_received_time, stats->puzzle_sent_time);
    }
    pthread_mutex_unlock(&stats->stat_lock);
}

void print_client_stats(ClientStats* stats) {
    pthread_mutex_lock(&stats->stat_lock);
    printf("CLIENT %d: Solved in %.2fs | Hints: %d | Correct: %s\n", 
           stats->client_id, stats->puzzle_solve_time, stats->hints_used, 
           stats->solved_correctly ? "YES" : "NO");
    pthread_mutex_unlock(&stats->stat_lock);
}

void update_global_stats(int solved) {
    pthread_mutex_lock(&stats_mutex);
    if (solved) {
        solved_games++;
    } else {
        abandoned_games++;
    }
    pthread_mutex_unlock(&stats_mutex);
}

/* ---------- Game Logic ---------- */
void evaluate_solution(char *solution, char *correct_solution, int hints_used, 
                       char *result, size_t result_size, int *is_correct) {
    int grid[9][9];
    for (int i = 0; i < 81; i++) grid[i / 9][i % 9] = solution[i] - '0';
    
    int valid_partial = check_sudoku_partial(grid);
    int complete = is_complete(grid);
    int exact = strcmp(solution, correct_solution) == 0;
    
    *is_correct = exact;
    
    if (exact) {
        if (hints_used == 0) snprintf(result, result_size, "PERFECT! Solved without hints!");
        else if (hints_used == 1) snprintf(result, result_size, "EXCELLENT! Solved with 1 hint.");
        else snprintf(result, result_size, "GOOD! Solved with 2 hints.");
    } else if (complete && !exact) {
        snprintf(result, result_size, "COMPLETE but incorrect.");
    } else if (valid_partial && !complete) {
        snprintf(result, result_size, "VALID partial solution.");
    } else {
        snprintf(result, result_size, "INVALID solution.");
    }
}

/* ---------- Worker Thread ---------- */
void* handle_client(void* arg) {
    QueueNode* node = (QueueNode*)arg;
    int sock = node->socket;
    int cid = node->client_id;
    struct Game game = node->game;
    char cid_str[32];
    snprintf(cid_str, sizeof(cid_str), "[CLIENT-%d]", cid);
    char log_buf[256];
    
    write_log(cid_str, "Processing started. Semaphore slot acquired.");
    ClientStats* stats = create_client_stats(cid, game.puzzle);
    
    pthread_mutex_lock(&stats_mutex);
    connected_clients++;
    games_in_progress++; 
    pthread_mutex_unlock(&stats_mutex);
    
    // Send ID
    if (server_send(sock, &cid, sizeof(cid)) != sizeof(cid)) {
        write_log(cid_str, "Error sending ID. Closing.");
        close_connection(sock); free(node); free(stats);
        goto cleanup;
    }
    snprintf(log_buf, sizeof(log_buf), "Sent Client ID: %d", cid);
    write_log(cid_str, log_buf);

    // Send Puzzle
    if (server_send(sock, game.puzzle, 81) != 81) {
        write_log(cid_str, "Error sending puzzle. Closing.");
        close_connection(sock); free(node); free(stats);
        goto cleanup;
    }
    write_log(cid_str, "Sent Puzzle.");
    update_stats_puzzle_sent(stats);
    
    char tournament_offer[] = "Join tournament? (y/n): ";
    server_send(sock, tournament_offer, strlen(tournament_offer) + 1);
    write_log(cid_str, "Sent Tournament Invitation.");
    
    char response[10] = {0};
    recv(sock, response, sizeof(response) - 1, 0);
    snprintf(log_buf, sizeof(log_buf), "Received response: %c", response[0]);
    write_log(cid_str, log_buf);

    int in_tournament = 0;
    if (response[0] == 'y' || response[0] == 'Y') {
        in_tournament = 1;
        join_tournament(&tournament, cid, sock);
        
        if (!keep_running) {
             write_log(cid_str, "Server shutdown during wait.");
             close_connection(sock); free(node); free(stats);
             goto cleanup;
        }
        
       char start_msg[64];
       snprintf(start_msg, sizeof(start_msg), "START %d", TOURNAMENT_TIME);
       server_send(sock, start_msg, strlen(start_msg) + 1);

        write_log(cid_str, "Sent START signal.");
    } else {
        char start_msg[] = "PLAY!";
        server_send(sock, start_msg, strlen(start_msg) + 1);
        write_log(cid_str, "Sent PLAY signal (Single Player).");
    }
    
    int has_left_tournament = 0;
    while (keep_running) {
        char solution[82] = {0};
        if (server_recv_all(sock, solution, 81) != 81) {
            write_log(cid_str, "Client disconnected or error receiving solution.");
            break; 
        }
        
        snprintf(log_buf, sizeof(log_buf), "Received Solution (starts with: %.10s...)", solution);
        write_log(cid_str, log_buf);
        
        char hint_char;
        if (recv(sock, &hint_char, 1, 0) != 1) break;

         if (hint_char < '0' || hint_char > '2') {
         write_log(cid_str, "Invalid hints value received.");
         break;
   }

int hints_used = hint_char - '0';
        snprintf(log_buf, sizeof(log_buf), "Received Hints Used: %d", hints_used);
        write_log(cid_str, log_buf);
        
        char result[256];
        int is_correct;
        evaluate_solution(solution, game.solution, hints_used, result, sizeof(result), &is_correct);
        update_stats_solution_received(stats, hints_used, is_correct);
        
        if (server_send(sock, result, strlen(result) + 1) < 0) break;
        
        snprintf(log_buf, sizeof(log_buf), "Sent Result: %s", result);
        write_log(cid_str, log_buf);
        print_client_stats(stats);
        
        if (is_correct) {
            update_global_stats(1);
            if (in_tournament) {
                add_to_leaderboard(cid, 1, stats->puzzle_solve_time);
                leave_tournament(&tournament);
                has_left_tournament = 1;
                in_tournament = 0;
                print_leaderboard();
            }
            break; 
        }
    }
    
    if (in_tournament && !has_left_tournament) {
        write_log(cid_str, "Client disconnected from tournament prematurely.");
        leave_tournament(&tournament);
    }
    
    close_connection(sock);
    free(node);
    free(stats);

cleanup:
    pthread_mutex_lock(&stats_mutex); 
    if (games_in_progress > 0) games_in_progress--; 
    if (connected_clients > 0) connected_clients--; 
    pthread_mutex_unlock(&stats_mutex);
    
    sem_post(&active_clients_sem);
    write_log(cid_str, "Session Ended. Semaphore released.");
    return NULL;
}

/* ---------- Dispatcher Thread ---------- */
void* dispatcher_thread(void* arg) {
    (void)arg;
    while (keep_running) {
        QueueNode* node = dequeue(&client_queue);
        if (node == NULL || !keep_running) break;
        
        pthread_mutex_lock(&stats_mutex); waiting_for_slot++; pthread_mutex_unlock(&stats_mutex);
        sem_wait(&active_clients_sem);
        pthread_mutex_lock(&stats_mutex); waiting_for_slot--; pthread_mutex_unlock(&stats_mutex);

        if (!keep_running) { free(node); break; }
        
        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, node) != 0) {
            sem_post(&active_clients_sem);
            free(node);
        } else {
            pthread_detach(tid);
        }
    }
    return NULL;
}

/* ---------- Acceptor Thread ---------- */
void* acceptor_thread(void* arg) {
    (void)arg;
    while (keep_running) {
        int cs = accept_client(server_fd_global);
        if (cs < 0) {
            if (!keep_running) break;
            continue;
        }
        
        client_counter++;
        int game_idx = rand() % total_games;
        
        char log_buf[128];
        snprintf(log_buf, sizeof(log_buf), "[MAIN] Connection accepted. Assigned Client-%d", client_counter);
        write_log("SERVER", log_buf);

        if (enqueue(&client_queue, cs, client_counter, games[game_idx]) != 0) {
            write_log("SERVER", "Queue full. Connection dropped.");
            close_connection(cs);
        }
    }
    return NULL;
}

/* ---------- Main ---------- */
int main(void) {
    signal(SIGINT, handle_sigint);
    signal(SIGPIPE, SIG_IGN); 

    FILE *conf = fopen("server_config.txt", "r");
    if (!conf) exit(1);
    char games_file[64];
    fscanf(conf, "%63s", games_file);
    fclose(conf);
    
    FILE *f = fopen(games_file, "r");
    if (!f) exit(1);
    char line[256];
    fgets(line, sizeof(line), f);
    while (fgets(line, sizeof(line), f) && total_games < MAX_GAMES) {
        char *token = strtok(line, ",");
        if (token) strncpy(games[total_games].id, token, 9);
        token = strtok(NULL, ",");
        if (token) strncpy(games[total_games].puzzle, token, 81);
        token = strtok(NULL, ",\n");
        if (token) strncpy(games[total_games].solution, token, 81);
        total_games++;
    }
    fclose(f);

    init_queue(&client_queue, QUEUE_CAPACITY);
    init_tournament(&tournament, TOURNAMENT_SIZE);
    sem_init(&active_clients_sem, 0, MAX_ACTIVE_CLIENTS);

    pthread_mutex_init(&tournament_leaderboard.lock, NULL);
    tournament_leaderboard.count = 0;

    server_fd_global = start_server(PORT, 10);
    if (server_fd_global < 0) exit(1);
    
    printf("Server listening on %d. Ctrl+C to stop.\n", PORT);
    write_log("[SERVER]", "Server Started on 8082.");

    pthread_t acc_tid, disp_tid;
    pthread_create(&acc_tid, NULL, acceptor_thread, NULL);
    pthread_create(&disp_tid, NULL, dispatcher_thread, NULL);
    
    while (keep_running) {
        sleep(10);
        display_queue_status(&client_queue);
    }
    
    pthread_join(acc_tid, NULL);
    pthread_join(disp_tid, NULL);
    
    if (server_fd_global >= 0) close(server_fd_global);
    
    sem_destroy(&active_clients_sem);
    destroy_tournament(&tournament);
    pthread_mutex_destroy(&client_queue.queue_lock);
    sem_destroy(&client_queue.slots_available);
    sem_destroy(&client_queue.items_available);
    
    printf("Server Shutdown Complete.\n");
    write_log("[SERVER]", "Server Shutdown Complete.");
    return 0;
}