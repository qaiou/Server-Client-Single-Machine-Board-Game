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

typedef struct {
    int socket;
    char name[NAME_SIZE];
    int total_score;
    int round_lives;
    int round_eliminated;
    int ready;
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
    int turn_in_progress;
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

typedef struct {
    char player_name[NAME_SIZE];
    int wins;
} ScoreRecord;

typedef struct {
    ScoreRecord records[100];
    int count;
    pthread_mutex_t lock;
    pthread_mutexattr_t lock_attr;
} ScoreData;

GameState *game = NULL;
LogBuffer *log_buffer = NULL;
ScoreData *score_data = NULL;
pthread_t logging_thread;
pthread_t scheduler_thread;
int logging_active = 1;
int scheduler_active = 1;
FILE *log_file = NULL;

const char *word_database[WORD_DATABASE_SIZE] = {
    "LEMON", "APPLE", "GRAPE", "MANGO", "PEACH",
    "ORANGE", "BANANA", "CHERRY", "MELON", "PAPAYA"
};

void add_log(const char *format, ...) {
    if (!log_buffer) return;
    pthread_mutex_lock(&log_buffer->lock);
    if (log_buffer->count < 2000) {
        va_list args;
        va_start(args, format);
        vsnprintf(log_buffer->entries[log_buffer->count].message, 512, format, args);
        va_end(args);
        log_buffer->entries[log_buffer->count].timestamp = time(NULL);
        log_buffer->count++;
    }
    pthread_mutex_unlock(&log_buffer->lock);
}

void *logger_func(void *arg) {
    log_file = fopen("game.log", "a");
    if (!log_file) return NULL;
    
    time_t now = time(NULL);
    fprintf(log_file, "\n=== GAME SESSION STARTED ===\n");
    fprintf(log_file, "Time: %s\n", ctime(&now));
    fflush(log_file);
    
    int last = 0;
    while (logging_active || last < log_buffer->count) {
        pthread_mutex_lock(&log_buffer->lock);
        while (last < log_buffer->count) {
            LogEntry *e = &log_buffer->entries[last];
            char *t = ctime(&e->timestamp);
            if (t) {
                t[strlen(t)-1] = '\0';
                fprintf(log_file, "[%s] %s\n", t, e->message);
            }
            fflush(log_file);
            last++;
        }
        pthread_mutex_unlock(&log_buffer->lock);
        usleep(50000);
    }
    
    fprintf(log_file, "=== SESSION END ===\n\n");
    fclose(log_file);
    return NULL;
}

void load_scores() {
    pthread_mutex_lock(&score_data->lock);
    score_data->count = 0;
    
    FILE *f = fopen("scores.txt", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f) && score_data->count < 100) {
            char name[NAME_SIZE];
            int wins;
            if (sscanf(line, "%[^,],%d", name, &wins) == 2) {
                strcpy(score_data->records[score_data->count].player_name, name);
                score_data->records[score_data->count].wins = wins;
                score_data->count++;
            }
        }
        fclose(f);
        add_log("Loaded %d player records from scores.txt", score_data->count);
    } else {
        add_log("scores.txt not found, will create new file");
    }
    pthread_mutex_unlock(&score_data->lock);
}

void save_scores() {
    pthread_mutex_lock(&score_data->lock);
    
    FILE *f = fopen("scores.txt", "w");
    if (f) {
        for (int i = 0; i < score_data->count; i++) {
            fprintf(f, "%s,%d\n", 
                    score_data->records[i].player_name,
                    score_data->records[i].wins);
        }
        fclose(f);
        add_log("Saved %d player records to scores.txt", score_data->count);
    }
    
    pthread_mutex_unlock(&score_data->lock);
}

void update_winner(const char *name) {
    pthread_mutex_lock(&score_data->lock);
    
    int found = 0;
    for (int i = 0; i < score_data->count; i++) {
        if (strcmp(score_data->records[i].player_name, name) == 0) {
            score_data->records[i].wins++;
            found = 1;
            add_log("Updated %s wins to %d", name, score_data->records[i].wins);
            break;
        }
    }
    
    if (!found && score_data->count < 100) {
        strcpy(score_data->records[score_data->count].player_name, name);
        score_data->records[score_data->count].wins = 1;
        score_data->count++;
        add_log("Added new winner: %s", name);
    }
    
    pthread_mutex_unlock(&score_data->lock);
}

void send_msg(int sock, const char *msg) {
    if (sock <= 0) return;
    char buf[512];
    snprintf(buf, sizeof(buf), "%s\n", msg);
    send(sock, buf, strlen(buf), MSG_NOSIGNAL);
}

void broadcast(const char *msg) {
    for (int i = 0; i < game->player_count; i++) {
        if (game->players[i].connected) {
            send_msg(game->players[i].socket, msg);
        }
    }
}

void send_board() {
    char msg[100];
    snprintf(msg, sizeof(msg), "BOARD:%s", game->answer_space);
    broadcast(msg);
    add_log("Broadcast board: %s", game->answer_space);
}

void send_state(int idx) {
    if (idx < 0 || idx >= game->player_count) return;
    char msg[100];
    // CRITICAL: Include elimination status in state message
    snprintf(msg, sizeof(msg), "STATE:R%d|L%d|S%d|E%d", 
             game->round, 
             game->players[idx].round_lives, 
             game->players[idx].total_score,
             game->players[idx].round_eliminated);  // E0=active, E1=eliminated
    send_msg(game->players[idx].socket, msg);
    add_log("Sent state to %s: R%d L%d S%d E%d", 
            game->players[idx].name, game->round, 
            game->players[idx].round_lives, 
            game->players[idx].total_score,
            game->players[idx].round_eliminated);
}

void broadcast_states() {
    for (int i = 0; i < game->player_count; i++) {
        if (game->players[i].connected) {
            send_state(i);
        }
    }
}

void init_answer() {
    int len = strlen(game->word);
    for (int i = 0; i < len; i++) {
        game->answer_space[i] = '_';
    }
    game->answer_space[len] = '\0';
}

void select_word() {
    srand(time(NULL) + game->round * 123);
    int idx = rand() % WORD_DATABASE_SIZE;
    strcpy(game->word, word_database[idx]);
    add_log("Round %d: Selected word %s", game->round, game->word);
}

int check_letter(char c) {
    c = toupper(c);
    if (!isalpha(c)) return -1;
    
    int found = 0;
    for (int i = 0; i < strlen(game->word); i++) {
        if (game->word[i] == c && game->answer_space[i] == '_') {
            found = 1;
        }
    }
    return found ? 1 : 0;
}

void update_answer(char c) {
    c = toupper(c);
    for (int i = 0; i < strlen(game->word); i++) {
        if (game->word[i] == c && game->answer_space[i] == '_') {
            game->answer_space[i] = c;
        }
    }
}

int is_complete() {
    return strchr(game->answer_space, '_') == NULL;
}

void init_round() {
    select_word();
    init_answer();
    
    for (int i = 0; i < game->player_count; i++) {
        game->players[i].ready = 0;
        game->players[i].round_lives = 3;
        game->players[i].round_eliminated = 0;  // RESET elimination
    }
    
    game->turn_in_progress = 0;
    
    add_log("Round %d initialized - ALL players reset to 3 lives, not eliminated", game->round);
}

int active_count() {
    int cnt = 0;
    for (int i = 0; i < game->player_count; i++) {
        if (!game->players[i].round_eliminated && game->players[i].connected) {
            cnt++;
        }
    }
    return cnt;
}

void show_scores() {
    char msg[512] = "ROUND_SCORES:";
    for (int i = 0; i < game->player_count; i++) {
        char info[100];
        if (game->players[i].round_eliminated) {
            snprintf(info, sizeof(info), "%s: ELIMINATED (Total: %d pts)", 
                     game->players[i].name, game->players[i].total_score);
        } else {
            snprintf(info, sizeof(info), "%s: %d pts (%d lives)", 
                     game->players[i].name, 
                     game->players[i].total_score, 
                     game->players[i].round_lives);
        }
        if (i > 0) strcat(msg, "|");
        strcat(msg, info);
    }
    broadcast(msg);
}

void save_final_results() {
    FILE *f = fopen("final_scores.txt", "w");
    if (!f) return;
    
    time_t now = time(NULL);
    fprintf(f, "=== GAME FINAL RESULTS ===\n");
    fprintf(f, "Date: %s\n", ctime(&now));
    fprintf(f, "Total Rounds: %d\n\n", TOTAL_ROUNDS);
    
    Player sorted[MAX_CLIENTS];
    memcpy(sorted, game->players, sizeof(Player) * game->player_count);
    
    for (int i = 0; i < game->player_count - 1; i++) {
        for (int j = i + 1; j < game->player_count; j++) {
            if (sorted[j].total_score > sorted[i].total_score) {
                Player tmp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = tmp;
            }
        }
    }
    
    fprintf(f, "RANKINGS:\n");
    for (int i = 0; i < game->player_count; i++) {
        fprintf(f, "%d. %s - %d points\n", 
                i+1, sorted[i].name, sorted[i].total_score);
    }
    
    fprintf(f, "\nWINNER: %s with %d points!\n", sorted[0].name, sorted[0].total_score);
    fclose(f);
    
    update_winner(sorted[0].name);
    save_scores();
    
    add_log("Game completed - Winner: %s (%d pts)", sorted[0].name, sorted[0].total_score);
}

int next_player() {
    int start = game->current_player;
    int next = (game->current_player + 1) % game->player_count;
    
    while (next != start) {
        if (!game->players[next].round_eliminated && game->players[next].connected) {
            return next;
        }
        next = (next + 1) % game->player_count;
    }
    
    if (!game->players[start].round_eliminated && game->players[start].connected) {
        return start;
    }
    return -1;
}

void handle_move(int idx, const char *move) {
    Player *p = &game->players[idx];
    
    add_log("%s handling move: %s", p->name, move);
    
    if (strncmp(move, "LETTER:", 7) == 0) {
        char letter = move[7];
        int result = check_letter(letter);
        
        if (result == -1) {
            send_msg(p->socket, "INVALID");
            add_log("%s: invalid letter", p->name);
        } else if (result == 1) {
            p->total_score++;
            update_answer(letter);
            send_msg(p->socket, "CORRECT_LETTER");
            add_log("%s: correct letter %c (+1 pt, total %d)", 
                    p->name, letter, p->total_score);
            
            usleep(100000);
            send_board();
            broadcast_states();
        } else {
            p->round_lives--;
            send_msg(p->socket, "WRONG_LETTER");
            add_log("%s: wrong letter %c (-1 life, %d left)", 
                    p->name, letter, p->round_lives);
            
            if (p->round_lives <= 0) {
                p->round_eliminated = 1;
                send_msg(p->socket, "ELIMINATED");
                add_log("%s: eliminated (no lives left in round %d)", 
                        p->name, game->round);
            }
            
            broadcast_states();
        }
    }
    else if (strncmp(move, "WORD:", 5) == 0) {
        char word[WORD_LEN];
        strcpy(word, move + 5);
        for (int i = 0; word[i]; i++) word[i] = toupper(word[i]);
        
        if (strcmp(word, game->word) == 0) {
            p->total_score += 3;
            strcpy(game->answer_space, game->word);
            send_msg(p->socket, "CORRECT_WORD");
            add_log("%s: correct word (+3 pts, total %d)", p->name, p->total_score);
            
            usleep(100000);
            send_board();
            broadcast_states();
        } else {
            p->round_eliminated = 1;
            p->round_lives = 0;
            send_msg(p->socket, "WRONG_WORD");
            add_log("%s: wrong word guess - eliminated from round %d", 
                    p->name, game->round);
            send_state(idx);
        }
    }
    
    p->ready = 1;
}

void timeout_handler(int idx) {
    sleep(TIMEOUT_SECONDS);
    
    pthread_mutex_lock(&game->lock);
    Player *p = &game->players[idx];
    
    if (!p->ready && game->current_player == idx) {
        p->total_score--;
        p->ready = 1;
        add_log("%s: timed out (-1 pt, total %d)", p->name, p->total_score);
        send_msg(p->socket, "TIMEOUT");
        send_state(idx);  // Send state to sync client
    }
    pthread_mutex_unlock(&game->lock);
    exit(0);
}

void client_handler(int idx) {
    int sock = game->players[idx].socket;
    char buf[256];
    
    memset(buf, 0, sizeof(buf));
    int n = recv(sock, buf, sizeof(buf)-1, 0);
    if (n <= 0) {
        close(sock);
        exit(0);
    }
    
    buf[strcspn(buf, "\r\n")] = 0;
    if (strncmp(buf, "NAME:", 5) != 0) {
        close(sock);
        exit(0);
    }
    
    pthread_mutex_lock(&game->lock);
    strcpy(game->players[idx].name, buf + 5);
    game->players[idx].total_score = 0;
    game->players[idx].round_lives = 3;
    game->players[idx].round_eliminated = 0;
    game->players[idx].connected = 1;
    pthread_mutex_unlock(&game->lock);
    
    add_log("Player %s connected (slot %d)", game->players[idx].name, idx);
    
    while (!game->game_started) usleep(100000);
    
    sleep(1);
    send_board();
    send_state(idx);
    
    while (!game->game_finished) {
        pthread_mutex_lock(&game->lock);
        
        if (game->current_player == idx && 
            !game->players[idx].round_eliminated && 
            !game->players[idx].ready &&
            !game->turn_in_progress) {
            
            game->turn_in_progress = 1;
            
            char turn_msg[100];
            snprintf(turn_msg, sizeof(turn_msg), "TURN:%s", game->players[idx].name);
            pthread_mutex_unlock(&game->lock);
            
            broadcast(turn_msg);
            sleep(1);
            send_msg(sock, "PROMPT");
            
            pid_t timeout_pid = fork();
            if (timeout_pid == 0) {
                timeout_handler(idx);
            }
            
            fd_set fds;
            struct timeval tv;
            FD_ZERO(&fds);
            FD_SET(sock, &fds);
            tv.tv_sec = TIMEOUT_SECONDS + 1;
            tv.tv_usec = 0;
            
            int ready = select(sock + 1, &fds, NULL, NULL, &tv);
            
            if (ready > 0) {
                memset(buf, 0, sizeof(buf));
                n = recv(sock, buf, sizeof(buf)-1, 0);
                
                if (n > 0) {
                    buf[strcspn(buf, "\r\n")] = 0;
                    kill(timeout_pid, SIGKILL);
                    waitpid(timeout_pid, NULL, 0);
                    
                    add_log("%s: received move %s", game->players[idx].name, buf);
                    
                    pthread_mutex_lock(&game->lock);
                    handle_move(idx, buf);
                    game->turn_in_progress = 0;
                    pthread_mutex_unlock(&game->lock);
                } else {
                    pthread_mutex_lock(&game->lock);
                    game->players[idx].connected = 0;
                    game->players[idx].round_eliminated = 1;
                    game->players[idx].ready = 1;
                    game->turn_in_progress = 0;
                    pthread_mutex_unlock(&game->lock);
                    kill(timeout_pid, SIGKILL);
                    waitpid(timeout_pid, NULL, 0);
                    break;
                }
            } else {
                waitpid(timeout_pid, NULL, 0);
                pthread_mutex_lock(&game->lock);
                game->turn_in_progress = 0;
                pthread_mutex_unlock(&game->lock);
            }
        } else {
            pthread_mutex_unlock(&game->lock);
            usleep(200000);
        }
    }
    
    add_log("Player %s disconnected", game->players[idx].name);
    close(sock);
    exit(0);
}

void *scheduler_func(void *arg) {
    add_log("Round Robin scheduler started");
    
    while (scheduler_active && !game->game_finished) {
        pthread_mutex_lock(&game->lock);
        
        if (!game->game_started) {
            pthread_mutex_unlock(&game->lock);
            usleep(100000);
            continue;
        }
        
        int curr = game->current_player;
        Player *p = &game->players[curr];
        
        if (p->ready) {
            add_log("Turn complete for %s", p->name);
            
            if (is_complete() || active_count() <= 0) {
                add_log("Round %d complete", game->round);
                
                char reveal[100];
                snprintf(reveal, sizeof(reveal), "REVEAL:%s", game->word);
                pthread_mutex_unlock(&game->lock);
                
                broadcast(reveal);
                sleep(3);
                
                pthread_mutex_lock(&game->lock);
                show_scores();
                pthread_mutex_unlock(&game->lock);
                sleep(4);
                
                pthread_mutex_lock(&game->lock);
                game->round++;
                
                if (game->round <= TOTAL_ROUNDS) {
                    add_log("Starting round %d/%d", game->round, TOTAL_ROUNDS);
                    
                    init_round();  // This resets round_eliminated to 0
                    
                    game->current_player = 0;
                    while (game->current_player < game->player_count && 
                           !game->players[game->current_player].connected) {
                        game->current_player++;
                    }
                    
                    pthread_mutex_unlock(&game->lock);
                    
                    sleep(1);
                    send_board();
                    broadcast_states();  // Send states with E0 (not eliminated)
                    
                    add_log("Round %d ready", game->round);
                    
                    pthread_mutex_lock(&game->lock);
                } else {
                    add_log("All %d rounds completed", TOTAL_ROUNDS);
                    game->game_finished = 1;
                    pthread_mutex_unlock(&game->lock);
                    broadcast("END");
                    save_final_results();
                    pthread_mutex_lock(&game->lock);
                }
            } else {
                int nxt = next_player();
                if (nxt >= 0) {
                    game->current_player = nxt;
                    game->players[nxt].ready = 0;
                    add_log("Turn advanced to %s", game->players[nxt].name);
                } else {
                    game->game_finished = 1;
                }
            }
        }
        
        pthread_mutex_unlock(&game->lock);
        usleep(100000);
    }
    
    add_log("Scheduler ended");
    return NULL;
}

void sigchld_handler(int sig) {
    int saved = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved;
}

void sigint_handler(int sig) {
    printf("\n\nShutting down server...\n");
    
    if (game) {
        game->game_finished = 1;
        broadcast("END");
        if (game->round > 1) {
            save_final_results();
        }
    }
    
    logging_active = 0;
    scheduler_active = 0;
    
    add_log("Server shutdown via SIGINT");
    sleep(1);
    
    exit(0);
}

int main() {
    int server_fd, new_sock;
    struct sockaddr_in addr;
    int addrlen = sizeof(addr);
    
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGINT, sigint_handler);
    
    game = mmap(NULL, sizeof(GameState), PROT_READ|PROT_WRITE, 
                MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    log_buffer = mmap(NULL, sizeof(LogBuffer), PROT_READ|PROT_WRITE, 
                      MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    score_data = mmap(NULL, sizeof(ScoreData), PROT_READ|PROT_WRITE,
                      MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    
    if (game == MAP_FAILED || log_buffer == MAP_FAILED || score_data == MAP_FAILED) {
        perror("mmap failed");
        exit(1);
    }
    
    pthread_mutexattr_init(&game->lock_attr);
    pthread_mutexattr_setpshared(&game->lock_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&game->lock, &game->lock_attr);
    
    pthread_mutexattr_init(&log_buffer->lock_attr);
    pthread_mutexattr_setpshared(&log_buffer->lock_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&log_buffer->lock, &log_buffer->lock_attr);
    
    pthread_mutexattr_init(&score_data->lock_attr);
    pthread_mutexattr_setpshared(&score_data->lock_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&score_data->lock, &score_data->lock_attr);
    
    log_buffer->count = 0;
    game->player_count = 0;
    game->current_player = 0;
    game->round = 1;
    game->game_started = 0;
    game->game_finished = 0;
    game->turn_in_progress = 0;
    
    pthread_create(&logging_thread, NULL, logger_func, NULL);
    
    load_scores();
    add_log("Server initialized");
    
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(1);
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        exit(1);
    }
    
    if (listen(server_fd, 3) < 0) {
        perror("listen failed");
        exit(1);
    }
    
    printf("╔════════════════════════════════════════╗\n");
    printf("║   Word Guessing Server Started         ║\n");
    printf("║   Port: %d                              ║\n", PORT);
    printf("║   Waiting for %d players...            ║\n", MAX_CLIENTS);
    printf("╚════════════════════════════════════════╝\n\n");
    
    add_log("Server listening on port %d", PORT);
    
    while (game->player_count < MAX_CLIENTS) {
        if ((new_sock = accept(server_fd, (struct sockaddr *)&addr, 
                              (socklen_t*)&addrlen)) < 0) {
            perror("accept failed");
            continue;
        }
        
        pthread_mutex_lock(&game->lock);
        int idx = game->player_count;
        game->players[idx].socket = new_sock;
        game->players[idx].ready = 0;
        game->players[idx].connected = 0;
        game->player_count++;
        pthread_mutex_unlock(&game->lock);
        
        printf("Connection %d accepted\n", idx + 1);
        add_log("Connection %d accepted", idx + 1);
        
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork failed");
            close(new_sock);
            game->player_count--;
        } else if (pid == 0) {
            close(server_fd);
            client_handler(idx);
            exit(0);
        }
    }
    
    printf("\n╔════════════════════════════════════════╗\n");
    printf("║   All %d players connected!            ║\n", MAX_CLIENTS);
    printf("╚════════════════════════════════════════╝\n\n");
    
    int all_ready = 0;
    while (!all_ready) {
        pthread_mutex_lock(&game->lock);
        all_ready = 1;
        for (int i = 0; i < game->player_count; i++) {
            if (!game->players[i].connected) {
                all_ready = 0;
                break;
            }
        }
        pthread_mutex_unlock(&game->lock);
        if (!all_ready) usleep(100000);
    }
    
    printf("Players:\n");
    for (int i = 0; i < game->player_count; i++) {
        printf("  %d. %s\n", i+1, game->players[i].name);
    }
    
    sleep(2);
    printf("\nStarting game - %d rounds total...\n\n", TOTAL_ROUNDS);
    
    init_round();
    
    pthread_create(&scheduler_thread, NULL, scheduler_func, NULL);
    
    game->game_started = 1;
    add_log("Game started with %d players", game->player_count);
    
    while (!game->game_finished) {
        sleep(1);
    }
    
    printf("\n╔════════════════════════════════════════╗\n");
    printf("║          Game Finished!                ║\n");
    printf("║   Check final_scores.txt for results   ║\n");
    printf("╚════════════════════════════════════════╝\n\n");
    
    sleep(2);
    
    scheduler_active = 0;
    pthread_join(scheduler_thread, NULL);
    
    logging_active = 0;
    pthread_join(logging_thread, NULL);
    
    close(server_fd);
    
    pthread_mutex_destroy(&game->lock);
    pthread_mutex_destroy(&log_buffer->lock);
    pthread_mutex_destroy(&score_data->lock);
    
    munmap(game, sizeof(GameState));
    munmap(log_buffer, sizeof(LogBuffer));
    munmap(score_data, sizeof(ScoreData));
    
    printf("Server shutdown complete. Ready for next game.\n");
    
    return 0;
}