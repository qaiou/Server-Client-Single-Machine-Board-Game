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

#define PORT 8080
#define MAX_CLIENTS 3
#define WORD_LEN 20
#define NAME_SIZE 50
#define ANSWER_SIZE 50
#define TOTAL_ROUNDS 5
#define TIMEOUT_SECONDS 15
#define WORD_DATABASE_SIZE 10

// ============================================
// NURA - Logging structures and mutex
// ============================================
typedef struct {
    char message[512];
    time_t timestamp;
} LogEntry;

typedef struct {
    LogEntry entries[1000];
    int count;
    pthread_mutex_t lock;
} LogBuffer;

LogBuffer log_buffer;
FILE *log_file;
pthread_t logging_thread;
int logging_active = 1;

// ============================================
// Game structures
// ============================================
typedef struct {
    int socket;
    char name[NAME_SIZE];
    int score;
    int lives;
    int is_eliminated;
    int ready;
    int timed_out;
} Player;

typedef struct {
    char word[WORD_LEN];
    char answer_space[ANSWER_SIZE];
    int current_player;
    int round;
    Player players[MAX_CLIENTS];
    int player_count;
    pthread_mutex_t lock;
    int game_started;
    int game_finished;
} GameState;

GameState game;

// ============================================
// QAISARA - Word database
// ============================================
const char *word_database[WORD_DATABASE_SIZE] = {
    "LEMON", "APPLE", "GRAPE", "MANGO", "PEACH",
    "ORANGE", "BANANA", "CHERRY", "MELON", "PAPAYA"
};

// ============================================
// NURA - Function 7: Logging functions
// ============================================
void add_log_entry(const char *format, ...) {
    pthread_mutex_lock(&log_buffer.lock);
    
    if (log_buffer.count < 1000) {
        va_list args;
        va_start(args, format);
        vsnprintf(log_buffer.entries[log_buffer.count].message, 
                  512, format, args);
        va_end(args);
        
        log_buffer.entries[log_buffer.count].timestamp = time(NULL);
        log_buffer.count++;
    }
    
    pthread_mutex_unlock(&log_buffer.lock);
}

void *logging_thread_func(void *arg) {
    log_file = fopen("server_log.txt", "w");
    if (!log_file) {
        perror("Failed to open log file");
        return NULL;
    }
    
    fprintf(log_file, "=== GAME SERVER LOG ===\n\n");
    fflush(log_file);
    
    int last_processed = 0;
    
    while (logging_active || last_processed < log_buffer.count) {
        pthread_mutex_lock(&log_buffer.lock);
        
        while (last_processed < log_buffer.count) {
            LogEntry *entry = &log_buffer.entries[last_processed];
            char *time_str = ctime(&entry->timestamp);
            time_str[strlen(time_str) - 1] = '\0';
            
            fprintf(log_file, "[%s] %s\n", time_str, entry->message);
            fflush(log_file);
            
            last_processed++;
        }
        
        pthread_mutex_unlock(&log_buffer.lock);
        
        usleep(100000); // 100ms
    }
    
    fclose(log_file);
    return NULL;
}

// ============================================
// Communication functions
// ============================================
void send_to_client(int socket, const char *msg) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "%s\n", msg);
    send(socket, buffer, strlen(buffer), 0);
    add_log_entry("Sent to socket %d: %s", socket, msg);
}

void broadcast(const char *msg) {
    pthread_mutex_lock(&game.lock);
    for (int i = 0; i < game.player_count; i++) {
        send_to_client(game.players[i].socket, msg);
    }
    pthread_mutex_unlock(&game.lock);
    add_log_entry("Broadcast: %s", msg);
}

void broadcast_board() {
    char msg[100];
    snprintf(msg, sizeof(msg), "BOARD:%s", game.answer_space);
    broadcast(msg);
}

void send_state_to_player(int player_idx) {
    char msg[100];
    snprintf(msg, sizeof(msg), "STATE:R%d|L%d|S%d", 
             game.round, 
             game.players[player_idx].lives, 
             game.players[player_idx].score);
    send_to_client(game.players[player_idx].socket, msg);
}

void broadcast_all_states() {
    for (int i = 0; i < game.player_count; i++) {
        send_state_to_player(i);
    }
}

// ============================================
// QAI - Function 1: void answerSpaces()
// ============================================
void answerSpaces() {
    int len = strlen(game.word);
    for (int i = 0; i < len; i++) {
        game.answer_space[i] = '_';
    }
    game.answer_space[len] = '\0';
    
    add_log_entry("Answer spaces initialized for word: %s (length: %d)", 
                  game.word, len);
}

// ============================================
// QAI - Function 6: char[] getword()
// ============================================
void getword() {
    srand(time(NULL) + game.round);
    int index = rand() % WORD_DATABASE_SIZE;
    strcpy(game.word, word_database[index]);
    
    add_log_entry("Selected word: %s for round %d", game.word, game.round);
}

// ============================================
// YUNIE - Function 2: Boolean isCorrect()
// ============================================
int isCorrect(char letter) {
    letter = toupper(letter);
    
    if (!isalpha(letter)) {
        add_log_entry("Invalid character: %c", letter);
        return -1; // Invalid input
    }
    
    int found = 0;
    for (int i = 0; i < strlen(game.word); i++) {
        if (game.word[i] == letter && game.answer_space[i] == '_') {
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
    for (int i = 0; i < strlen(game.word); i++) {
        if (game.word[i] == letter && game.answer_space[i] == '_') {
            game.answer_space[i] = letter;
            updated++;
        }
    }
    
    add_log_entry("Updated answer spaces with letter %c (%d positions)", 
                  letter, updated);
}

// ============================================
// NUHA - Function 4: Boolean wordIsComplete()
// ============================================
int wordIsComplete() {
    int complete = (strchr(game.answer_space, '_') == NULL);
    
    if (complete) {
        add_log_entry("Word is complete: %s", game.answer_space);
    }
    
    return complete;
}

// ============================================
// Game helper functions
// ============================================
void initialize_round() {
    getword(); // QAI - Function 6
    answerSpaces(); // QAI - Function 1
    
    add_log_entry("Round %d initialized", game.round);
}

int count_active_players() {
    int count = 0;
    for (int i = 0; i < game.player_count; i++) {
        if (!game.players[i].is_eliminated) {
            count++;
        }
    }
    return count;
}

void show_round_scores() {
    char msg[512];
    snprintf(msg, sizeof(msg), "ROUND_SCORES:");
    
    for (int i = 0; i < game.player_count; i++) {
        char player_info[100];
        if (game.players[i].is_eliminated) {
            snprintf(player_info, sizeof(player_info), "%s: ELIMINATED", 
                     game.players[i].name);
        } else {
            snprintf(player_info, sizeof(player_info), "%s: %d points (%d lives)", 
                     game.players[i].name, 
                     game.players[i].score, 
                     game.players[i].lives);
        }
        
        if (i > 0) strcat(msg, "|");
        strcat(msg, player_info);
    }
    
    broadcast(msg);
    add_log_entry("Round %d scores displayed", game.round);
}

void save_final_scores() {
    FILE *f = fopen("scores.txt", "w");
    if (!f) return;
    
    fprintf(f, "=== FINAL GAME SCORES ===\n\n");
    
    Player sorted[MAX_CLIENTS];
    memcpy(sorted, game.players, sizeof(Player) * game.player_count);
    
    for (int i = 0; i < game.player_count - 1; i++) {
        for (int j = i + 1; j < game.player_count; j++) {
            if (sorted[j].score > sorted[i].score) {
                Player temp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = temp;
            }
        }
    }
    
    for (int i = 0; i < game.player_count; i++) {
        fprintf(f, "%d. %s - %d points (%d lives remaining)\n", 
                i + 1, sorted[i].name, sorted[i].score, sorted[i].lives);
    }
    
    fclose(f);
    add_log_entry("Final scores saved");
}

// ============================================
// Timeout monitoring
// ============================================
void *timeout_monitor(void *arg) {
    int player_idx = *((int *)arg);
    free(arg);
    
    sleep(TIMEOUT_SECONDS);
    
    pthread_mutex_lock(&game.lock);
    
    if (!game.players[player_idx].ready && !game.players[player_idx].timed_out) {
        game.players[player_idx].timed_out = 1;
        game.players[player_idx].score--;
        
        add_log_entry("Player %s timed out", game.players[player_idx].name);
        
        send_to_client(game.players[player_idx].socket, "TIMEOUT");
        send_state_to_player(player_idx);
        
        game.players[player_idx].ready = 1;
    }
    
    pthread_mutex_unlock(&game.lock);
    return NULL;
}

// ============================================
// YUNIE - Function 5: Handle player move with scoring
// ============================================
void handle_player_move(int player_idx, const char *move) {
    pthread_mutex_lock(&game.lock);
    
    Player *p = &game.players[player_idx];
    
    if (p->timed_out) {
        p->ready = 1;
        pthread_mutex_unlock(&game.lock);
        return;
    }
    
    if (strncmp(move, "LETTER:", 7) == 0) {
        char letter = move[7];
        
        int result = isCorrect(letter); // YUNIE - Function 2
        
        if (result == -1) {
            send_to_client(p->socket, "INVALID");
            p->ready = 1;
            pthread_mutex_unlock(&game.lock);
            return;
        }
        
        if (result == 1) {
            // Correct guess
            p->score++; // YUNIE - +1 mark for correct letter
            updateAnswerSpaces(letter); // NUHA - Function 3
            send_to_client(p->socket, "CORRECT");
            add_log_entry("Player %s guessed letter %c correctly (+1 point)", 
                          p->name, letter);
        } else {
            // Wrong guess
            p->lives--; // YUNIE - -1 life for wrong letter
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
        
        if (strcmp(word, game.word) == 0) {
            // Correct word
            p->score += 3; // YUNIE - +3 marks for correct word
            strcpy(game.answer_space, game.word);
            send_to_client(p->socket, "CORRECT");
            add_log_entry("Player %s guessed word correctly (+3 points)", p->name);
            
            broadcast_board();
            broadcast_all_states();
        } else {
            // Wrong word - ELIMINATION
            p->is_eliminated = 1;
            p->lives = 0;
            send_to_client(p->socket, "ELIMINATED");
            add_log_entry("Player %s guessed word incorrectly - ELIMINATED", p->name);
            
            send_state_to_player(player_idx);
        }
    }
    
    p->ready = 1;
    pthread_mutex_unlock(&game.lock);
}

// ============================================
// QAISARA - Round Robin Scheduler
// ============================================
int get_next_player_round_robin() {
    int start_player = game.current_player;
    int next_player = (game.current_player + 1) % game.player_count;
    
    // Round-robin: find next active (non-eliminated) player
    while (next_player != start_player) {
        if (!game.players[next_player].is_eliminated) {
            add_log_entry("Round-robin: Next player is %s (index %d)", 
                          game.players[next_player].name, next_player);
            return next_player;
        }
        next_player = (next_player + 1) % game.player_count;
    }
    
    // Check if start_player is still active
    if (!game.players[start_player].is_eliminated) {
        return start_player;
    }
    
    // No active players
    return -1;
}

// ============================================
// Client handler
// ============================================
void *client_handler(void *arg) {
    int player_idx = *((int *)arg);
    free(arg);
    
    int sock = game.players[player_idx].socket;
    char buffer[256];
    
    // Wait for name
    memset(buffer, 0, sizeof(buffer));
    int valread = recv(sock, buffer, sizeof(buffer), 0);
    if (valread <= 0 || strncmp(buffer, "NAME:", 5) != 0) {
        close(sock);
        return NULL;
    }
    
    buffer[strcspn(buffer, "\r\n")] = 0;
    strcpy(game.players[player_idx].name, buffer + 5);
    game.players[player_idx].lives = 3;
    game.players[player_idx].score = 0;
    game.players[player_idx].is_eliminated = 0;
    
    add_log_entry("Player %s joined (socket %d)", 
                  game.players[player_idx].name, sock);
    
    // Wait for game to start
    while (!game.game_started) {
        usleep(100000);
    }
    
    // Send initial game state
    sleep(1);
    broadcast_board();
    send_state_to_player(player_idx);
    
    // Main game loop - only current player processes
    while (!game.game_finished) {
        pthread_mutex_lock(&game.lock);
        
        if (game.current_player == player_idx && !game.players[player_idx].is_eliminated) {
            game.players[player_idx].ready = 0;
            game.players[player_idx].timed_out = 0;
            
            // Broadcast turn
            char turn_msg[100];
            snprintf(turn_msg, sizeof(turn_msg), "TURN:%s", 
                     game.players[player_idx].name);
            broadcast(turn_msg);
            
            pthread_mutex_unlock(&game.lock);
            
            sleep(1);
            
            // Prompt current player
            send_to_client(sock, "PROMPT");
            
            // Send wait to others
            for (int i = 0; i < game.player_count; i++) {
                if (i != player_idx) {
                    send_to_client(game.players[i].socket, "WAIT");
                }
            }
            
            // Start timeout thread
            pthread_t timeout_thread;
            int *idx = malloc(sizeof(int));
            *idx = player_idx;
            pthread_create(&timeout_thread, NULL, timeout_monitor, idx);
            pthread_detach(timeout_thread);
            
            // Wait for move
            memset(buffer, 0, sizeof(buffer));
            valread = recv(sock, buffer, sizeof(buffer), 0);
            
            if (valread > 0) {
                buffer[strcspn(buffer, "\r\n")] = 0;
                handle_player_move(player_idx, buffer);
            }
            
            // Wait until ready
            while (!game.players[player_idx].ready) {
                usleep(50000);
            }
            
            pthread_mutex_lock(&game.lock);
            
            // NUHA - Function 4: Check if word is complete
            if (wordIsComplete() || count_active_players() <= 0) {
                char reveal_msg[100];
                snprintf(reveal_msg, sizeof(reveal_msg), "REVEAL:%s", game.word);
                broadcast(reveal_msg);
                
                pthread_mutex_unlock(&game.lock);
                sleep(2);
                pthread_mutex_lock(&game.lock);
                
                show_round_scores();
                
                pthread_mutex_unlock(&game.lock);
                sleep(3);
                pthread_mutex_lock(&game.lock);
                
                game.round++;
                if (game.round <= TOTAL_ROUNDS) {
                    initialize_round();
                    broadcast_board();
                    broadcast_all_states();
                    game.current_player = 0; // Reset to player 1
                } else {
                    game.game_finished = 1;
                }
            } else {
                // QAISARA - Round-robin to next player
                int next = get_next_player_round_robin();
                if (next >= 0) {
                    game.current_player = next;
                } else {
                    game.game_finished = 1;
                }
            }
            
            pthread_mutex_unlock(&game.lock);
        } else {
            pthread_mutex_unlock(&game.lock);
            usleep(200000);
        }
    }
    
    // Game ended
    pthread_mutex_lock(&game.lock);
    static int end_sent = 0;
    if (!end_sent) {
        broadcast("END");
        save_final_scores();
        end_sent = 1;
    }
    pthread_mutex_unlock(&game.lock);
    
    add_log_entry("Player %s finished game", game.players[player_idx].name);
    
    // Keep connection open for a bit
    sleep(3);
    close(sock);
    
    return NULL;
}

// ============================================
// Main function
// ============================================
int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    
    // NURA - Initialize logging
    log_buffer.count = 0;
    pthread_mutex_init(&log_buffer.lock, NULL);
    pthread_mutex_init(&game.lock, NULL);
    
    // NURA - Start logging thread
    pthread_create(&logging_thread, NULL, logging_thread_func, NULL);
    
    add_log_entry("Server starting...");
    
    // Initialize game
    game.player_count = 0;
    game.current_player = 0;
    game.round = 1;
    game.game_started = 0;
    game.game_finished = 0;
    
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
    
    printf("Server listening on port %d\n", PORT);
    printf("Waiting for %d players to connect...\n", MAX_CLIENTS);
    add_log_entry("Server listening on port %d", PORT);
    
    // Accept connections
    while (game.player_count < MAX_CLIENTS) {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, 
                                (socklen_t *)&addrlen)) < 0) {
            perror("accept");
            continue;
        }
        
        game.players[game.player_count].socket = new_socket;
        
        int *idx = malloc(sizeof(int));
        *idx = game.player_count;
        
        pthread_t thread_id;
        pthread_create(&thread_id, NULL, client_handler, idx);
        pthread_detach(thread_id);
        
        game.player_count++;
        printf("Player %d connected\n", game.player_count);
        add_log_entry("Player %d connected", game.player_count);
    }
    
    sleep(2);
    
    // Start game
    printf("\nAll players connected! Starting game...\n");
    add_log_entry("Starting game with %d players", game.player_count);
    
    initialize_round();
    game.game_started = 1;
    
    // Wait for game to finish
    while (!game.game_finished) {
        sleep(1);
    }
    
    printf("\nGame finished! Cleaning up...\n");
    sleep(5);
    
    close(server_fd);
    
    // NURA - Stop logging thread
    logging_active = 0;
    pthread_join(logging_thread, NULL);
    
    pthread_mutex_destroy(&log_buffer.lock);
    pthread_mutex_destroy(&game.lock);
    
    printf("Server shutting down.\n");
    add_log_entry("Server shutdown complete");
    
    return 0;
}