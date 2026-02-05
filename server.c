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
}

void sendToClient(GameState *g, int idx, const char *msg) {
    send(g->clientSockets[idx], msg, strlen(msg), 0);
}

void broadcast(GameState *g, const char *msg) {
    for (int i = 0; i < MAX_PLAYERS; i++)
        sendToClient(g, i, msg);
}

// -------- Game Logic --------
void chooseRandomWord(GameState *g) {
    strcpy(g->answerWord, words[rand() % wordCount]);
    printf("\n New word: %s\n", g->answerWord);
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
    sprintf(msg, "BOARD:%s", g->answerSpace);
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
    if (correct)
        sendBoard(g);
    return correct;
}

int wordIsComplete(GameState *g) {
    for (int i = 0; i < strlen(g->answerWord); i++)
        if (g->answerSpace[i] == '_')
            return 0;
    return 1;
}

// -------- Protocol Signals --------
void sendPrompt(GameState *g, int idx) { sendToClient(g, idx, "PROMPT"); }
void sendWait(GameState *g, int idx)   { sendToClient(g, idx, "WAIT"); }
void sendInvalid(GameState *g, int idx){ sendToClient(g, idx, "INVALID"); }
void sendCorrect(GameState *g, int idx){ sendToClient(g, idx, "CORRECT"); }
void sendWrong(GameState *g, int idx)  { sendToClient(g, idx, "WRONG"); }

void startNewRound(GameState *g) {
    chooseRandomWord(g);
    initRound(g);
    sendBoard(g);

    // Force turn ownership
    sendPrompt(g, g->currentPlayer);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i != g->currentPlayer)
            sendWait(g, i);
    }

    pthread_cond_broadcast(&g->turnCond);
}

void sendReveal(GameState *g) {
    char msg[64];
    sprintf(msg, "REVEAL:%s", g->answerWord);
    broadcast(g, msg);
}

void sendEnd(GameState *g) {
    broadcast(g, "END");
}

// -------- Client Handler --------
void handleClient(int me, GameState *g) {
    char buffer[256];

    while (1) {
        pthread_mutex_lock(&g->mutex);
        while (!g->gameOver && g->currentPlayer != me)
            pthread_cond_wait(&g->turnCond, &g->mutex);

        if (g->gameOver) {
            pthread_mutex_unlock(&g->mutex);
            break;
        }

        sendPrompt(g, me);
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
            printf("%s timed out\n", g->names[me]);
            g->currentPlayer = (g->currentPlayer + 1) % MAX_PLAYERS;
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
        int n = read(g->clientSockets[me], buffer, sizeof(buffer));
        if (n <= 0) break;

        if (strncmp(buffer, "MOVE:", 5) != 0)
            continue;

        pthread_mutex_lock(&g->mutex);

        g->guessLetter[me] = buffer[5];

        if (!isalpha(g->guessLetter[me])) {
            sendInvalid(g, me);
            pthread_mutex_unlock(&g->mutex);
            continue;
        }

        if (applyGuess(g, me)) {  //if guess is correect
            sendCorrect(g, me);
            g->scores[me]++;
        } else {                    //if guess is wrong
            sendWrong(g, me);
            g->lives[me]--;
        }

        if (wordIsComplete(g)) {
            g->round++;
            
            sendReveal(g);

            if (g->round >= MAX_ROUNDS) {
                g->gameOver = 1;
                sendEnd(g);
                pthread_cond_broadcast(&g->turnCond);
                pthread_mutex_unlock(&g->mutex);
                break;
            }

            startNewRound(g);
            pthread_mutex_unlock(&g->mutex);
            continue;
        }

        g->currentPlayer = (g->currentPlayer + 1) % MAX_PLAYERS;
        pthread_cond_broadcast(&g->turnCond);
        pthread_mutex_unlock(&g->mutex);
    }

    close(g->clientSockets[me]);
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
        g->clientSockets[i] = accept(serverFd, (struct sockaddr *)&address, &addrlen);
        char buf[256] = {0};
        read(g->clientSockets[i], buf, sizeof(buf));
        if (strncmp(buf, "NAME:", 5) == 0)
            strncpy(g->names[i], buf + 5, NAME_SIZE - 1);

        printf("Player %d: %s\n", i + 1, g->names[i]);
    }

    printf("\nGame starting!\n");
    chooseRandomWord(g);
    initRound(g);
    sendBoard(g);

    sendPrompt(g, 0);
    sendWait(g, 1);
    sendWait(g, 2);

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (fork() == 0) {
            close(serverFd);
            handleClient(i, g);
        }
    }

    while (1) pause();
    return 0;
}
