#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <time.h>
#include <sys/select.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

#define PORT 8080
#define MAX_CLIENTS 3
#define WORD_LEN 20
#define NAME_SIZE 50
#define ANSWER_SIZE 50
#define TOTAL_ROUNDS 5
#define TIMEOUT_SECONDS 15
#define WORD_DATABASE_SIZE 10

// ============================================
// Shared memory structures
// ============================================
typedef struct {
    int socket;
    char name[NAME_SIZE];
    int score;
    int lives;
    int is_eliminated;
    int ready;
    int timed_out;
    int waiting_for_input;
    pid_t pid;
    int connected;
} Player;

typedef struct {
    char word[WORD_LEN];
    char answer_space[ANSWER_SIZE];
    int current_player;
    int round;
    Player players[MAX_CLIENTS];
    int player_count;
    int game_started;
    int game_finished;
    pthread_mutex_t lock;
    pthread_mutexattr_t lock_attr;
} GameState;

typedef struct {
    char message[512];
    time_t timestamp;
} LogEntry;

typedef struct {
    LogEntry entries[2000];
    int count;
    pthread_mutex_t lock;
    pthread_mutexattr_t lock_attr;
} LogBuffer;

// Global pointers to shared memory
GameState *game = NULL;
LogBuffer *log_buffer = NULL;

// Thread control
pthread_t logging_thread;
pthread_t scheduler_thread;
int logging_active = 1;
int scheduler_active = 1;

// File for persistent logging
FILE *log_file = NULL;

// ============================================
// Word database
// ============================================
const char *word_database[WORD_DATABASE_SIZE] = {
    "LEMON", "APPLE", "GRAPE", "MANGO", "PEACH",
    "ORANGE", "BANANA", "CHERRY", "MELON", "PAPAYA"
};

// ============================================
// NURA - Function 7: Logging functions
// ============================================
void add_log_entry(const char *format, ...) {
    if (!log_buffer) return;
    
    pthread_mutex_lock(&log_buffer->lock);
    
    if (log_buffer->count < 2000) {
        va_list args;
        va_start(args, format);
        vsnprintf(log_buffer->entries[log_buffer->count].message, 
                  512, format, args);
        va_end(args);
        
        log_buffer->entries[log_buffer->count].timestamp = time(NULL);
        log_buffer->count++;
    }
    
    pthread_mutex_unlock(&log_buffer->lock);
}

void *logging_thread_func(void *arg) {
    log_file = fopen("game.log", "w");
    if (!log_file) {
        perror("Failed to open log file");
        return NULL;
    }
    
    fprintf(log_file, "=== WORD GUESSING GAME SERVER LOG ===\n");
    fprintf(log_file, "Started: %s\n\n", ctime(&(time_t){time(NULL)}));
    fflush(log_file);
    
    int last_processed = 0;
    
    while (logging_active || last_processed < log_buffer->count) {
        pthread_mutex_lock(&log_buffer->lock);
        
        while (last_processed < log_buffer->count) {
            LogEntry *entry = &log_buffer->entries[last_processed];
            char *time_str = ctime(&entry->timestamp);
            if (time_str) {
                time_str[strlen(time_str) - 1] = '\0';
                fprintf(log_file, "[%s] %s\n", time_str, entry->message);
            } else {
                fprintf(log_file, "[TIME ERROR] %s\n", entry->message);
            }
            fflush(log_file);
            
            last_processed++;
        }
        
        pthread_mutex_unlock(&log_buffer->lock);
        
       struct timespec ts = {0, 50000000}; // 0 seconds, 50 million nanoseconds (50ms)
       nanosleep(&ts,NULL);
    }
    
    fprintf(log_file, "\n=== LOG END ===\n");
    fclose(log_file);
    return NULL;
}

// ============================================
// Communication functions
// ============================================
void send_to_client(int socket, const char *msg) {
    if (socket <= 0) return;
    
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "%s\n", msg);
    int sent = send(socket, buffer, strlen(buffer), MSG_NOSIGNAL);
    if (sent < 0) {
        add_log_entry("Failed to send to socket %d: %s", socket, msg);
    }
}

void broadcast(const char *msg) {
    pthread_mutex_lock(&game->lock);
    for (int i = 0; i < game->player_count; i++) {
        if (game->players[i].connected) {
            send_to_client(game->players[i].socket, msg);
        }
    }
    pthread_mutex_unlock(&game->lock);
}

void broadcast_board() {
    char msg[100];
    snprintf(msg, sizeof(msg), "BOARD:%s", game->answer_space);
    broadcast(msg);
}

void send_state_to_player(int player_idx) {
    if (player_idx < 0 || player_idx >= game->player_count) return;
    
    char msg[100];
    snprintf(msg, sizeof(msg), "STATE:R%d|L%d|S%d", 
             game->round, 
             game->players[player_idx].lives, 
             game->players[player_idx].score);
    send_to_client(game->players[player_idx].socket, msg);
}

void broadcast_all_states() {
    for (int i = 0; i < game->player_count; i++) {
        if (game->players[i].connected) {
            send_state_to_player(i);
        }
    }
}

// ============================================
// QAI - Function 1: void answerSpaces()
// ============================================
void answerSpaces() {
    int len = strlen(game->word);
    for (int i = 0; i < len; i++) {
        game->answer_space[i] = '_';
    }
    game->answer_space[len] = '\0';
    
    add_log_entry("Answer spaces initialized for word: %s (length: %d)", 
                  game->word, len);
}

// ============================================
// QAI - Function 6: void getword()
// ============================================
void getword() {
    srand(time(NULL) + game->round * getpid());
    int index = rand() % WORD_DATABASE_SIZE;
    strcpy(game->word, word_database[index]);
    
    add_log_entry("Selected word: %s for round %d", game->word, game->round);
}

// ============================================
// YUNIE - Function 2: int isCorrect()
// ============================================
int isCorrect(char letter) {
    letter = toupper(letter);
    
    if (!isalpha(letter)) {
        add_log_entry("Invalid character: %c", letter);
        return -1;
    }
    
    int found = 0;
    for (int i = 0; i < strlen(game->word); i++) {
        if (game->word[i] == letter && game->answer_space[i] == '_') {
            found = 1;
            break;
        }
    }
    
    add_log_entry("Letter %c is %s", letter, found ? "CORRECT" : "WRONG");
    return found;
}

// ============================================
// NUHA - Function 3: void updateAnswerSpaces()
// ============================================
void updateAnswerSpaces(char letter) {
    letter = toupper(letter);
    
    int updated = 0;
    for (int i = 0; i < strlen(game->word); i++) {
        if (game->word[i] == letter && game->answer_space[i] == '_') {
            game->answer_space[i] = letter;
            updated++;
        }
    }
    
    add_log_entry("Updated answer spaces with letter %c (%d positions)", 
                  letter, updated);
}

// ============================================
// NUHA - Function 4: int wordIsComplete()
// ============================================
int wordIsComplete() {
    int complete = (strchr(game->answer_space, '_') == NULL);
    
    if (complete) {
        add_log_entry("Word is complete: %s", game->answer_space);
    }
    
    return complete;
}

// ============================================
// Game helper functions
// ============================================
void initialize_round() {
    getword();
    answerSpaces();
    
    // Reset ready flags
    for (int i = 0; i < game->player_count; i++) {
        game->players[i].ready = 0;
        game->players[i].timed_out = 0;
        game->players[i].waiting_for_input = 0;
    }
    
    add_log_entry("Round %d initialized", game->round);
}

int count_active_players() {
    int count = 0;
    for (int i = 0; i < game->player_count; i++) {
        if (!game->players[i].is_eliminated && game->players[i].connected) {
            count++;
        }
    }
    return count;
}

void show_round_scores() {
    char msg[512];
    snprintf(msg, sizeof(msg), "ROUND_SCORES:");
    
    for (int i = 0; i < game->player_count; i++) {
        char player_info[100];
        if (game->players[i].is_eliminated) {
            snprintf(player_info, sizeof(player_info), "%s: ELIMINATED", 
                     game->players[i].name);
        } else {
            snprintf(player_info, sizeof(player_info), "%s: %d points (%d lives)", 
                     game->players[i].name, 
                     game->players[i].score, 
                     game->players[i].lives);
        }
        
        if (i > 0) strcat(msg, "|");
        strcat(msg, player_info);
    }
    
    broadcast(msg);
    add_log_entry("Round %d scores displayed", game->round);
}

void save_final_scores() {
    FILE *f = fopen("scores.txt", "w");
    if (!f) return;
    
    time_t now = time(NULL);
    fprintf(f, "=== FINAL GAME SCORES ===\n");
    fprintf(f, "Date: %s\n", ctime(&now));
    
    Player sorted[MAX_CLIENTS];
    memcpy(sorted, game->players, sizeof(Player) * game->player_count);
    
    for (int i = 0; i < game->player_count - 1; i++) {
        for (int j = i + 1; j < game->player_count; j++) {
            if (sorted[j].score > sorted[i].score) {
                Player temp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = temp;
            }
        }
    }
    
    for (int i = 0; i < game->player_count; i++) {
        fprintf(f, "%d. %s - %d points (%d lives remaining)\n", 
                i + 1, sorted[i].name, sorted[i].score, sorted[i].lives);
    }
    
    fclose(f);
    add_log_entry("Final scores saved to scores.txt");
}

// ============================================
// QAISARA - Round Robin Scheduler Thread
// ============================================
int get_next_player_round_robin() {
    int start_player = game->current_player;
    int next_player = (game->current_player + 1) % game->player_count;
    
    while (next_player != start_player) {
        if (!game->players[next_player].is_eliminated && 
            game->players[next_player].connected) {
            add_log_entry("Round-robin: Next player is %s (index %d)", 
                          game->players[next_player].name, next_player);
            return next_player;
        }
        next_player = (next_player + 1) % game->player_count;
    }
    
    if (!game->players[start_player].is_eliminated && 
        game->players[start_player].connected) {
        return start_player;
    }
    
    return -1;
}

void *scheduler_thread_func(void *arg) {
    add_log_entry("Scheduler thread started");
    
    while (scheduler_active && !game->game_finished) {
        pthread_mutex_lock(&game->lock);
        
        if (game->game_started && !game->game_finished) {
            int current = game->current_player;
            
            if (current >= 0 && current < game->player_count) {
                Player *p = &game->players[current];
                
                // Check if turn is complete
                if (p->ready && !game->game_finished) {
                    add_log_entry("Scheduler: Player %s (idx %d) completed turn (ready=1)", 
                                 p->name, current);
                    
                    if (wordIsComplete() || count_active_players() <= 0) {
                        add_log_entry("Scheduler: Round complete - word finished or no active players");
                        
                        char reveal_msg[100];
                        snprintf(reveal_msg, sizeof(reveal_msg), "REVEAL:%s", game->word);
                        
                        pthread_mutex_unlock(&game->lock);
                        broadcast(reveal_msg);
                        sleep(2);
                        pthread_mutex_lock(&game->lock);
                        
                        show_round_scores();
                        
                        pthread_mutex_unlock(&game->lock);
                        sleep(3);
                        pthread_mutex_lock(&game->lock);
                        
                        game->round++;
                        if (game->round <= TOTAL_ROUNDS && count_active_players() > 0) {
                            add_log_entry("Scheduler: Starting round %d", game->round);
                            initialize_round();
                            broadcast_board();
                            broadcast_all_states();
                            
                            // Find first active player
                            game->current_player = 0;
                            while (game->current_player < game->player_count &&
                                   game->players[game->current_player].is_eliminated) {
                                game->current_player++;
                            }
                            
                            add_log_entry("Scheduler: Round %d starts with player %s (idx %d)", 
                                         game->round, 
                                         game->players[game->current_player].name,
                                         game->current_player);
                        } else {
                            add_log_entry("Scheduler: Game finished - total rounds reached or no players");
                            game->game_finished = 1;
                            pthread_mutex_unlock(&game->lock);
                            broadcast("END");
                            save_final_scores();
                            pthread_mutex_lock(&game->lock);
                        }
                    } else {
                        // Move to next player
                        int prev_player = current;
                        int next = get_next_player_round_robin();
                        
                        if (next >= 0) {
                            game->current_player = next;
                            game->players[next].ready = 0;
                            game->players[next].timed_out = 0;
                            game->players[next].waiting_for_input = 0;
                            
                            add_log_entry("Scheduler: Turn advanced from %s (idx %d) to %s (idx %d)", 
                                         game->players[prev_player].name, prev_player,
                                         game->players[next].name, next);
                        } else {
                            add_log_entry("Scheduler: No next player found - ending game");
                            game->game_finished = 1;
                        }
                    }
                }
            }
        }
        
        pthread_mutex_unlock(&game->lock);
        usleep(50000); // 50ms check interval
    }
    
    add_log_entry("Scheduler thread ended");
    return NULL;
}

// ============================================
// YUNIE - Function 5: Handle player move
// ============================================
void handle_player_move(int player_idx, const char *move) {
    pthread_mutex_lock(&game->lock);
    
    Player *p = &game->players[player_idx];
    
    if (p->timed_out) {
        p->ready = 1;
        pthread_mutex_unlock(&game->lock);
        return;
    }
    
    if (strncmp(move, "LETTER:", 7) == 0) {
        char letter = move[7];
        
        int result = isCorrect(letter);
        
        if (result == -1) {
            send_to_client(p->socket, "INVALID");
            p->ready = 1;
            pthread_mutex_unlock(&game->lock);
            return;
        }
        
        if (result == 1) {
            p->score++;
            updateAnswerSpaces(letter);
            send_to_client(p->socket, "CORRECT");
            add_log_entry("Player %s guessed letter %c correctly (+1 point)", 
                          p->name, letter);
        } else {
            p->lives--;
            send_to_client(p->socket, "WRONG");
            add_log_entry("Player %s guessed letter %c incorrectly (-1 life)", 
                          p->name, letter);
            
            if (p->lives <= 0) {
                p->is_eliminated = 1;
                send_to_client(p->socket, "ELIMINATED");
                add_log_entry("Player %s eliminated (no lives)", p->name);
            }
        }
        
        broadcast_board();
        broadcast_all_states();
    }
    else if (strncmp(move, "WORD:", 5) == 0) {
        char word[WORD_LEN];
        strcpy(word, move + 5);
        
        for (int i = 0; word[i]; i++) {
            word[i] = toupper(word[i]);
        }
        
        if (strcmp(word, game->word) == 0) {
            p->score += 3;
            strcpy(game->answer_space, game->word);
            send_to_client(p->socket, "CORRECT");
            add_log_entry("Player %s guessed word correctly (+3 points)", p->name);
            
            broadcast_board();
            broadcast_all_states();
        } else {
            p->is_eliminated = 1;
            p->lives = 0;
            send_to_client(p->socket, "ELIMINATED");
            add_log_entry("Player %s guessed word incorrectly - ELIMINATED", p->name);
            
            send_state_to_player(player_idx);
        }
    }
    
    p->ready = 1;
    p->waiting_for_input = 0;
    pthread_mutex_unlock(&game->lock);
}

// ============================================
// Timeout monitor (separate process)
// ============================================
void timeout_monitor_process(int player_idx) {
    sleep(TIMEOUT_SECONDS);
    
    pthread_mutex_lock(&game->lock);
    
    Player *p = &game->players[player_idx];
    
    if (p->waiting_for_input && !p->ready && !p->timed_out) {
        p->timed_out = 1;
        p->score--;
        p->ready = 1;
        p->waiting_for_input = 0;
        
        add_log_entry("Player %s timed out - penalty applied", p->name);
        
        send_to_client(p->socket, "TIMEOUT");
        send_state_to_player(player_idx);
    }
    
    pthread_mutex_unlock(&game->lock);
    exit(0);
}

// ============================================
// Client handler (runs in forked child process)
// ============================================
void client_process(int player_idx) {
    int sock = game->players[player_idx].socket;
    char buffer[256];
    
    add_log_entry("Child process started for player slot %d (PID: %d)", 
                  player_idx, getpid());
    
    // Receive player name
    memset(buffer, 0, sizeof(buffer));
    int valread = recv(sock, buffer, sizeof(buffer) - 1, 0);
    
    if (valread <= 0) {
        close(sock);
        exit(0);
    }
    
    buffer[strcspn(buffer, "\r\n")] = 0;
    
    if (strncmp(buffer, "NAME:", 5) != 0) {
        close(sock);
        exit(0);
    }
    
    pthread_mutex_lock(&game->lock);
    strcpy(game->players[player_idx].name, buffer + 5);
    game->players[player_idx].lives = 3;
    game->players[player_idx].score = 0;
    game->players[player_idx].is_eliminated = 0;
    game->players[player_idx].connected = 1;
    game->players[player_idx].pid = getpid();
    pthread_mutex_unlock(&game->lock);
    
    add_log_entry("Player %s joined (socket %d, PID %d)", 
                  game->players[player_idx].name, sock, getpid());
    
    // Wait for game to start
    while (!game->game_started) {
        usleep(100000);
    }
    
    // Send initial state
    sleep(1);
    broadcast_board();
    send_state_to_player(player_idx);
    
    // Main game loop
    while (!game->game_finished) {
        pthread_mutex_lock(&game->lock);
        
        // Check if it's this player's turn
        int is_my_turn = (game->current_player == player_idx && 
                         !game->players[player_idx].is_eliminated &&
                         !game->players[player_idx].ready);
        
        if (is_my_turn) {
            // Mark as waiting for input
            game->players[player_idx].waiting_for_input = 1;
            
            char turn_msg[100];
            snprintf(turn_msg, sizeof(turn_msg), "TURN:%s", 
                     game->players[player_idx].name);
            
            add_log_entry("It's %s's turn (player %d)", 
                         game->players[player_idx].name, player_idx);
            
            pthread_mutex_unlock(&game->lock);
            
            broadcast(turn_msg);
            usleep(500000); // 500ms delay for message to reach clients
            
            send_to_client(sock, "PROMPT");
            
            // Send wait to others
            pthread_mutex_lock(&game->lock);
            for (int i = 0; i < game->player_count; i++) {
                if (i != player_idx && game->players[i].connected) {
                    send_to_client(game->players[i].socket, "WAIT");
                }
            }
            pthread_mutex_unlock(&game->lock);
            
            // Fork timeout monitor
            pid_t timeout_pid = fork();
            if (timeout_pid == 0) {
                timeout_monitor_process(player_idx);
                exit(0);
            }
            
            // Wait for player input with timeout using select
            fd_set readfds;
            struct timeval tv;
            
            FD_ZERO(&readfds);
            FD_SET(sock, &readfds);
            tv.tv_sec = TIMEOUT_SECONDS + 1;
            tv.tv_usec = 0;
            
            int ready = select(sock + 1, &readfds, NULL, NULL, &tv);
            
            if (ready > 0) {
                // Data available
                memset(buffer, 0, sizeof(buffer));
                valread = recv(sock, buffer, sizeof(buffer) - 1, 0);
                
                if (valread > 0) {
                    buffer[strcspn(buffer, "\r\n")] = 0;
                    
                    // Kill timeout monitor - player responded
                    kill(timeout_pid, SIGKILL);
                    waitpid(timeout_pid, NULL, 0);
                    
                    add_log_entry("Player %s sent move: %s", 
                                  game->players[player_idx].name, buffer);
                    
                    handle_player_move(player_idx, buffer);
                    
                    // Small delay to let scheduler see the ready flag
                    usleep(100000); // 100ms
                } else {
                    // Connection lost
                    pthread_mutex_lock(&game->lock);
                    game->players[player_idx].connected = 0;
                    game->players[player_idx].is_eliminated = 1;
                    game->players[player_idx].ready = 1;
                    pthread_mutex_unlock(&game->lock);
                    
                    kill(timeout_pid, SIGKILL);
                    waitpid(timeout_pid, NULL, 0);
                    break;
                }
            } else {
                // Timeout occurred (handled by timeout_monitor_process)
                waitpid(timeout_pid, NULL, 0);
                
                // Small delay to let scheduler see the timeout
                usleep(100000); // 100ms
            }
            
        } else {
            pthread_mutex_unlock(&game->lock);
            usleep(200000); // 200ms - check less frequently when not my turn
        }
    }
    
    add_log_entry("Player %s finished game (PID %d)", 
                  game->players[player_idx].name, getpid());
    
    close(sock);
    exit(0);
}

// ============================================
// Signal handlers
// ============================================
void sigchld_handler(int sig) {
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved_errno;
}

void sigint_handler(int sig) {
    printf("\n\nShutting down server...\n");
    
    if (game) {
        game->game_finished = 1;
        broadcast("END");
        save_final_scores();
    }
    
    logging_active = 0;
    scheduler_active = 0;
    
    if (logging_thread) {
        pthread_join(logging_thread, NULL);
    }
    if (scheduler_thread) {
        pthread_join(scheduler_thread, NULL);
    }
    
    add_log_entry("Server shutdown by SIGINT");
    
    exit(0);
}

// ============================================
// Main function
// ============================================
int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    
    // Setup signal handlers
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    
    signal(SIGINT, sigint_handler);
    
    // Create shared memory for game state
    game = mmap(NULL, sizeof(GameState), 
                PROT_READ | PROT_WRITE, 
                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    
    if (game == MAP_FAILED) {
        perror("mmap game");
        exit(EXIT_FAILURE);
    }
    
    // Create shared memory for log buffer
    log_buffer = mmap(NULL, sizeof(LogBuffer), 
                      PROT_READ | PROT_WRITE, 
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    
    if (log_buffer == MAP_FAILED) {
        perror("mmap log_buffer");
        exit(EXIT_FAILURE);
    }
    
    // Initialize process-shared mutex for game
    pthread_mutexattr_init(&game->lock_attr);
    pthread_mutexattr_setpshared(&game->lock_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&game->lock, &game->lock_attr);
    
    // Initialize process-shared mutex for logging
    pthread_mutexattr_init(&log_buffer->lock_attr);
    pthread_mutexattr_setpshared(&log_buffer->lock_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&log_buffer->lock, &log_buffer->lock_attr);
    
    // Initialize buffers
    log_buffer->count = 0;
    game->player_count = 0;
    game->current_player = 0;
    game->round = 1;
    game->game_started = 0;
    game->game_finished = 0;
    
    // Start logging thread (in parent process)
    pthread_create(&logging_thread, NULL, logging_thread_func, NULL);
    
    add_log_entry("Server starting...");
    
    // Create socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    
    printf("╔════════════════════════════════════════╗\n");
    printf("║   Word Guessing Game Server Started    ║\n");
    printf("╠════════════════════════════════════════╣\n");
    printf("║ Port: %-32d ║\n", PORT);
    printf("║ Waiting for %d players...              ║\n", MAX_CLIENTS);
    printf("╚════════════════════════════════════════╝\n\n");
    
    add_log_entry("Server listening on port %d", PORT);
    
    // Accept client connections and fork child processes
    while (game->player_count < MAX_CLIENTS) {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, 
                                (socklen_t *)&addrlen)) < 0) {
            perror("accept");
            continue;
        }
        
        pthread_mutex_lock(&game->lock);
        int idx = game->player_count;
        game->players[idx].socket = new_socket;
        game->players[idx].ready = 0;
        game->players[idx].connected = 0;
        game->players[idx].waiting_for_input = 0;
        game->player_count++;
        pthread_mutex_unlock(&game->lock);
        
        printf("Connection %d accepted, forking child process...\n", idx + 1);
        add_log_entry("Connection %d accepted", idx + 1);
        
        // Fork child process to handle this client
        pid_t pid = fork();
        
        if (pid < 0) {
            perror("fork failed");
            close(new_socket);
            game->player_count--;
        } 
        else if (pid == 0) {
            // Child process
            close(server_fd);
            client_process(idx);
            exit(0);
        } 
        else {
            // Parent process
            // Child will use the socket
        }
    }
    
    // All players connected
    printf("\n╔════════════════════════════════════════╗\n");
    printf("║   All %d players connected!             ║\n", MAX_CLIENTS);
    printf("║   Waiting for player names...          ║\n");
    printf("╚════════════════════════════════════════╝\n\n");
    
    // Wait for all players to send names
    int all_connected = 0;
    while (!all_connected) {
        pthread_mutex_lock(&game->lock);
        all_connected = 1;
        for (int i = 0; i < game->player_count; i++) {
            if (!game->players[i].connected) {
                all_connected = 0;
                break;
            }
        }
        pthread_mutex_unlock(&game->lock);
        
        if (!all_connected) {
            usleep(100000);
        }
    }
    
    printf("\nPlayers:\n");
    for (int i = 0; i < game->player_count; i++) {
        printf("  %d. %s\n", i + 1, game->players[i].name);
    }
    
    sleep(2);
    
    // Initialize first round
    printf("\nStarting game...\n\n");
    add_log_entry("Starting game with %d players", game->player_count);
    
    initialize_round();
    
    // Start scheduler thread (in parent process)
    pthread_create(&scheduler_thread, NULL, scheduler_thread_func, NULL);
    
    game->game_started = 1;
    
    // Wait for game to finish
    while (!game->game_finished) {
        sleep(1);
    }
    
    printf("\n╔════════════════════════════════════════╗\n");
    printf("║          Game Finished!                ║\n");
    printf("║   Check scores.txt for final results   ║\n");
    printf("╚════════════════════════════════════════╝\n\n");
    
    sleep(3);
    
    // Cleanup
    scheduler_active = 0;
    pthread_join(scheduler_thread, NULL);
    
    logging_active = 0;
    pthread_join(logging_thread, NULL);
    
    close(server_fd);
    
    pthread_mutex_destroy(&game->lock);
    pthread_mutex_destroy(&log_buffer->lock);
    pthread_mutexattr_destroy(&game->lock_attr);
    pthread_mutexattr_destroy(&log_buffer->lock_attr);
    
    munmap(game, sizeof(GameState));
    munmap(log_buffer, sizeof(LogBuffer));
    
    printf("Server shutdown complete.\n");
    
    return 0;
}