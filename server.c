#include <stdio.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>
#include <ctype.h>
#include <signal.h>

#define PORT 8080
#define ANSWER_SIZE 6
#define NAME_SIZE 50
#define MAX_WORDS 110
#define WORD_LEN 8
#define MAX_PLAYERS 3
#define MAX_ROUNDS 5
#define LOG_FILE "game_log.txt"
#define SCORE_FILE "scores.txt"
#define LOG_QUEUE_SIZE 1000

// -------- Logging Thread Structures --------
typedef struct {
    char timestamp[32];
    char message[512];
} LogEntry;

typedef struct {
    LogEntry entries[LOG_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t notEmpty;
} LogQueue;

LogQueue logQueue = {0};

// -------- Shared Memory Game State --------
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t turnCond;

    char answerSpace[ANSWER_SIZE];
    char answerWord[WORD_LEN];

    int clientSockets[MAX_PLAYERS];
    char names[MAX_PLAYERS][NAME_SIZE];

    char guessLetter[MAX_PLAYERS];
    int lives[MAX_PLAYERS];
    int scores[MAX_PLAYERS];

    int currentPlayer;
    int round;
    int gameOver;

} GameState;

char words[MAX_WORDS][WORD_LEN];
int wordCount = 0;

// -------- Logging Utilities --------
void initLogQueue() {
    pthread_mutex_init(&logQueue.mutex, NULL);
    pthread_cond_init(&logQueue.notEmpty, NULL);
    logQueue.head = 0;
    logQueue.tail = 0;
    logQueue.count = 0;
}

void enqueueLog(const char *message) {
    time_t now = time(NULL);
    struct tm *timeinfo = localtime(&now);
    
    pthread_mutex_lock(&logQueue.mutex);
    
    if (logQueue.count >= LOG_QUEUE_SIZE) {
        pthread_mutex_unlock(&logQueue.mutex);
        fprintf(stderr, "Log queue full, dropping message: %s\n", message);
        return;
    }
    
    strftime(logQueue.entries[logQueue.tail].timestamp, sizeof(logQueue.entries[logQueue.tail].timestamp),
             "%Y-%m-%d %H:%M:%S", timeinfo);
    strncpy(logQueue.entries[logQueue.tail].message, message, sizeof(logQueue.entries[logQueue.tail].message) - 1);
    logQueue.entries[logQueue.tail].message[sizeof(logQueue.entries[logQueue.tail].message) - 1] = '\0';
    
    logQueue.tail = (logQueue.tail + 1) % LOG_QUEUE_SIZE;
    logQueue.count++;
    
    pthread_cond_signal(&logQueue.notEmpty);
    pthread_mutex_unlock(&logQueue.mutex);
}

void* loggingThread(void *arg) {
    FILE *logFile = fopen(LOG_FILE, "a");
    if (!logFile) {
        perror("Failed to open log file");
        return NULL;
    }
    
    LogEntry entry;
    
    while (1) {
        pthread_mutex_lock(&logQueue.mutex);
        
        // Wait until there's something to log
        while (logQueue.count == 0) {
            pthread_cond_wait(&logQueue.notEmpty, &logQueue.mutex);
        }
        
        // Dequeue a log entry
        entry = logQueue.entries[logQueue.head];
        logQueue.head = (logQueue.head + 1) % LOG_QUEUE_SIZE;
        logQueue.count--;
        
        pthread_mutex_unlock(&logQueue.mutex);
        
        // Write to file without holding the lock
        fprintf(logFile, "[%s] %s\n", entry.timestamp, entry.message);
        fflush(logFile);
    }
    
    fclose(logFile);
    return NULL;
}

// -------- Utilities --------
void loadWords(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        perror("Failed to open words file");
        exit(1);
    }

    while (fgets(words[wordCount], WORD_LEN, fp) && wordCount < MAX_WORDS) {
        words[wordCount][strcspn(words[wordCount], "\n")] = 0;
        wordCount++;
    }
    fclose(fp);
    
    char logMsg[256];
    snprintf(logMsg, sizeof(logMsg), "Loaded %d words from %s", wordCount, filename);
    enqueueLog(logMsg);
}

void sendLine(GameState *g, int idx, const char *msg) {
    send(g->clientSockets[idx], msg, strlen(msg), 0);
}

void broadcast(GameState *g, const char *msg) {
    for (int i = 0; i < MAX_PLAYERS; i++)
        sendLine(g, i, msg);
}

// -------- Game Logic --------
void chooseRandomWord(GameState *g) {
    strcpy(g->answerWord, words[rand() % wordCount]);
    printf("\n New word: %s\n", g->answerWord);
    
    char logMsg[256];
    snprintf(logMsg, sizeof(logMsg), "Round %d: New word chosen", g->round + 1);
    enqueueLog(logMsg);
}

void initRound(GameState *g) {
    int len = strlen(g->answerWord);
    for (int i = 0; i < len; i++)
        g->answerSpace[i] = '_';
    g->answerSpace[len] = '\0';

    for (int i = 0; i < MAX_PLAYERS; i++)
        g->lives[i] = 3;

    g->currentPlayer = 0;
}

void sendBoard(GameState *g) {
    char msg[64];
    sprintf(msg, "BOARD:%s\n", g->answerSpace);
    broadcast(g, msg);
}

int applyGuess(GameState *g, int player) {
    int correct = 0;
    for (int i = 0; i < strlen(g->answerWord); i++) {
        if (tolower(g->guessLetter[player]) == tolower(g->answerWord[i])) {
            g->answerSpace[i] = g->guessLetter[player];
            correct = 1;
        }
    }  
    return correct;
}

int wordIsComplete(GameState *g) {
    for (int i = 0; i < strlen(g->answerWord); i++)
        if (g->answerSpace[i] == '_')
            return 0;
    return 1;
}

// -------- Protocol Signals --------
void sendPrompt(GameState *g, int idx)  { sendLine(g, idx, "PROMPT\n"); }
void sendWait(GameState *g, int idx)    { sendLine(g, idx, "WAIT\n"); }
void sendInvalid(GameState *g, int idx) { sendLine(g, idx, "INVALID\n"); }
void sendCorrect(GameState *g, int idx) { sendLine(g, idx, "CORRECT\n"); }
void sendWrong(GameState *g, int idx)   { sendLine(g, idx, "WRONG\n"); }

void sendReveal(GameState *g) {
    char msg[64];
    sprintf(msg, "REVEAL:%s\n", g->answerWord);
    broadcast(g, msg);
}

void sendEnd(GameState *g) {
    broadcast(g, "END\n");
}

void startNewRound(GameState *g) {
    chooseRandomWord(g);
    initRound(g);
    sendBoard(g);

    sendPrompt(g, g->currentPlayer);
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (i != g->currentPlayer)
            sendWait(g, i);

    pthread_cond_broadcast(&g->turnCond);
}

// -------- Client Handler --------
void handleClient(int me, GameState *g) {
    char buffer[256];
    char logMsg[256];

    snprintf(logMsg, sizeof(logMsg), "Player %d (%s) connected", me, g->names[me]);
    enqueueLog(logMsg);

    while (1) {
        pthread_mutex_lock(&g->mutex);
        while (!g->gameOver && g->currentPlayer != me)
            pthread_cond_wait(&g->turnCond, &g->mutex);

        if (g->gameOver) {
            pthread_mutex_unlock(&g->mutex);
            break;
        }

        // Only send PROMPT on first round, not on subsequent turns
        // because startNewRound() handles it
        pthread_mutex_unlock(&g->mutex);

        // ----- 10s timeout -----
        fd_set readfds;
        struct timeval tv;
        FD_ZERO(&readfds);
        FD_SET(g->clientSockets[me], &readfds);
        tv.tv_sec = 10;
        tv.tv_usec = 0;

        int ready = select(g->clientSockets[me] + 1, &readfds, NULL, NULL, &tv);

        pthread_mutex_lock(&g->mutex);

        if (ready == 0) {
            printf("[SERVER] %s timed out\n", g->names[me]);
            snprintf(logMsg, sizeof(logMsg), "%s timed out (no response in 10s)", g->names[me]);
            enqueueLog(logMsg);
            g->currentPlayer = (g->currentPlayer + 1) % MAX_PLAYERS;
            
            // Send signals to other players
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (i == g->currentPlayer)
                    sendPrompt(g, i);
                else
                    sendWait(g, i);
            }
            
            pthread_cond_broadcast(&g->turnCond);
            pthread_mutex_unlock(&g->mutex);
            continue;
        }

        if (ready < 0) {
            perror("select");
            pthread_mutex_unlock(&g->mutex);
            break;
        }

        pthread_mutex_unlock(&g->mutex);

        memset(buffer, 0, sizeof(buffer));
        int n = recv(g->clientSockets[me], buffer, sizeof(buffer) - 1, 0);
        if (n <= 0) break;

        if (strncmp(buffer, "MOVE:", 5) != 0)
            continue;

        pthread_mutex_lock(&g->mutex);

        g->guessLetter[me] = buffer[5];

        if (!isalpha(g->guessLetter[me])) {
            sendInvalid(g, me);
            snprintf(logMsg, sizeof(logMsg), "%s guessed invalid character: %c", g->names[me], g->guessLetter[me]);
            enqueueLog(logMsg);
            pthread_mutex_unlock(&g->mutex);
            continue;
        }

        if (applyGuess(g, me)) {
            sendCorrect(g, me);
            sendBoard(g);
            g->scores[me]++;
            snprintf(logMsg, sizeof(logMsg), "%s guessed correct: %c (score: %d)", g->names[me], g->guessLetter[me], g->scores[me]);
            enqueueLog(logMsg);
        } else {
            sendWrong(g, me);
            sendBoard(g);
            g->lives[me]--;
            snprintf(logMsg, sizeof(logMsg), "%s guessed wrong: %c (lives remaining: %d)", g->names[me], g->guessLetter[me], g->lives[me]);
            enqueueLog(logMsg);
        }

        if (wordIsComplete(g)) {
            g->round++;
            sendReveal(g);
            snprintf(logMsg, sizeof(logMsg), "Round %d completed. Word was: %s", g->round, g->answerWord);
            enqueueLog(logMsg);

            if (g->round >= MAX_ROUNDS) {
                g->gameOver = 1;
                sendEnd(g);
                snprintf(logMsg, sizeof(logMsg), "Game over! Final scores - Player 0: %d, Player 1: %d, Player 2: %d",
                         g->scores[0], g->scores[1], g->scores[2]);
                enqueueLog(logMsg);
                pthread_cond_broadcast(&g->turnCond);
                pthread_mutex_unlock(&g->mutex);
                break;
            }

            startNewRound(g);
            pthread_mutex_unlock(&g->mutex);
            continue;
        }

        g->currentPlayer = (g->currentPlayer + 1) % MAX_PLAYERS;
        
        // Send PROMPT to new current player and WAIT to others
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (i == g->currentPlayer)
                sendPrompt(g, i);
            else
                sendWait(g, i);
        }
        
        pthread_cond_broadcast(&g->turnCond);
        pthread_mutex_unlock(&g->mutex);
    }

    close(g->clientSockets[me]);
    snprintf(logMsg, sizeof(logMsg), "Player %d (%s) disconnected", me, g->names[me]);
    enqueueLog(logMsg);
    exit(0);
}

// -------- Main --------
int main() {
    signal(SIGCHLD, SIG_IGN);

    int serverFd;
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);
    GameState *g = NULL;
    int shmFd;

    // Initialize logging
    initLogQueue();
    pthread_t logThread;
    pthread_create(&logThread, NULL, loggingThread, NULL);
    enqueueLog("===== Server started =====");

    if ((serverFd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(serverFd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    if (listen(serverFd, MAX_PLAYERS) < 0) {
        perror("listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", PORT);
    char logMsg[256];
    snprintf(logMsg, sizeof(logMsg), "Server listening on port %d", PORT);
    enqueueLog(logMsg);

    shmFd = shm_open("/wordgame_shm", O_CREAT | O_RDWR, 0666);
    ftruncate(shmFd, sizeof(GameState));
    g = mmap(NULL, sizeof(GameState), PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);

    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&g->mutex, &mattr);

    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&g->turnCond, &cattr);

    g->round = 0;
    g->gameOver = 0;

    loadWords("words.txt");
    srand(time(NULL));

    // ----- Accept players -----
    for (int i = 0; i < MAX_PLAYERS; i++) {
        printf("Waiting for player %d...\n", i + 1);
        snprintf(logMsg, sizeof(logMsg), "Waiting for player %d to connect...", i + 1);
        enqueueLog(logMsg);
        
        g->clientSockets[i] = accept(serverFd, (struct sockaddr *)&address, &addrlen);
        char buf[256] = {0};
        recv(g->clientSockets[i], buf, sizeof(buf) - 1, 0);
        if (strncmp(buf, "NAME:", 5) == 0)
            strncpy(g->names[i], buf + 5, NAME_SIZE - 1);

        printf("Player %d: %s\n", i + 1, g->names[i]);
        snprintf(logMsg, sizeof(logMsg), "Player %d connected: %s", i + 1, g->names[i]);
        enqueueLog(logMsg);
    }

    printf("\nGame starting!\n");
    enqueueLog("===== Game starting =====");
    chooseRandomWord(g);
    initRound(g);
    sendBoard(g);

    sendPrompt(g, 0);
    //sendWait(g, 1);
    //sendWait(g, 2);

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (fork() == 0) {
            close(serverFd);
            handleClient(i, g);
        }
    }

    while (1) pause();
    return 0;
}