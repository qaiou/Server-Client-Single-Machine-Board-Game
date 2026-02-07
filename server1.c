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

// ========================================
// SHARED DATA STRUCTURES - QAI
// ========================================
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t turnCond;

    char answerSpace[ANSWER_SIZE];
    char answerWord[WORD_LEN];

    int clientSockets[MAX_PLAYERS];
    char names[MAX_PLAYERS][NAME_SIZE];

    char guessLetter[MAX_PLAYERS];
    char guessWord[MAX_PLAYERS][WORD_LEN];
    int lives[MAX_PLAYERS];
    int scores[MAX_PLAYERS];

    int currentPlayer;
    int round;
    int gameOver;
    
    FILE *logFile;  // NURA - Log file pointer
} GameState;

char words[MAX_WORDS][WORD_LEN];
int wordCount = 0;

// ========================================
// LOGGING FUNCTIONS - NURA (Part 7)
// ========================================
void initLog(GameState *g) {
    g->logFile = fopen(LOG_FILE, "w");
    if (!g->logFile) {
        perror("Failed to open log file");
        exit(1);
    }
    
    time_t now = time(NULL);
    char *timeStr = ctime(&now);
    timeStr[strcspn(timeStr, "\n")] = 0;
    
    fprintf(g->logFile, "========================================\n");
    fprintf(g->logFile, "   WORD GUESSING GAME - SESSION LOG\n");
    fprintf(g->logFile, "========================================\n");
    fprintf(g->logFile, "Game Started: %s\n", timeStr);
    fprintf(g->logFile, "Max Rounds: %d\n", MAX_ROUNDS);
    fprintf(g->logFile, "Number of Players: %d\n", MAX_PLAYERS);
    fprintf(g->logFile, "========================================\n\n");
    fflush(g->logFile);
}

void logPlayers(GameState *g) {
    fprintf(g->logFile, "REGISTERED PLAYERS:\n");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        fprintf(g->logFile, "  Player %d: %s (Lives: %d, Score: %d)\n", 
                i + 1, g->names[i], g->lives[i], g->scores[i]);
    }
    fprintf(g->logFile, "\n");
    fflush(g->logFile);
}

void logRoundStart(GameState *g) {
    fprintf(g->logFile, "========================================\n");
    fprintf(g->logFile, "ROUND %d START\n", g->round + 1);
    fprintf(g->logFile, "========================================\n");
    fprintf(g->logFile, "Word to guess: %s (Length: %d)\n", g->answerWord, (int)strlen(g->answerWord));
    fprintf(g->logFile, "Initial board: %s\n", g->answerSpace);
    fprintf(g->logFile, "\nPlayer Status:\n");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        fprintf(g->logFile, "  %s - Lives: %d, Score: %d\n", 
                g->names[i], g->lives[i], g->scores[i]);
    }
    fprintf(g->logFile, "\n");
    fflush(g->logFile);
}

void logLetterGuess(GameState *g, int player, char guess, int correct) {
    time_t now = time(NULL);
    char *timeStr = ctime(&now);
    timeStr[strcspn(timeStr, "\n")] = 0;
    
    fprintf(g->logFile, "[%s] Turn: %s (LETTER GUESS)\n", timeStr, g->names[player]);
    fprintf(g->logFile, "  Guessed: '%c'\n", guess);
    fprintf(g->logFile, "  Result: %s\n", correct ? "CORRECT (+1 point)" : "WRONG (-1 life)");
    fprintf(g->logFile, "  Board: %s\n", g->answerSpace);
    fprintf(g->logFile, "  Lives remaining: %d\n", g->lives[player]);
    fprintf(g->logFile, "  Score: %d\n\n", g->scores[player]);
    fflush(g->logFile);
}

void logWordGuess(GameState *g, int player, const char *guess, int correct) {
    time_t now = time(NULL);
    char *timeStr = ctime(&now);
    timeStr[strcspn(timeStr, "\n")] = 0;
    
    fprintf(g->logFile, "[%s] Turn: %s (WORD GUESS)\n", timeStr, g->names[player]);
    fprintf(g->logFile, "  Guessed word: '%s'\n", guess);
    if (correct) {
        fprintf(g->logFile, "  Result: CORRECT! (+3 points, round ends)\n");
    } else {
        fprintf(g->logFile, "  Result: WRONG! (ELIMINATED - lost all lives)\n");
    }
    fprintf(g->logFile, "  Lives remaining: %d\n", g->lives[player]);
    fprintf(g->logFile, "  Score: %d\n\n", g->scores[player]);
    fflush(g->logFile);
}

void logTimeout(GameState *g, int player) {
    time_t now = time(NULL);
    char *timeStr = ctime(&now);
    timeStr[strcspn(timeStr, "\n")] = 0;
    
    fprintf(g->logFile, "[%s] TIMEOUT: %s failed to respond within 10 seconds (-1 point)\n", 
            timeStr, g->names[player]);
    fprintf(g->logFile, "  Score: %d\n\n", g->scores[player]);
    fflush(g->logFile);
}

void logInvalidGuess(GameState *g, int player, const char *guess) {
    time_t now = time(NULL);
    char *timeStr = ctime(&now);
    timeStr[strcspn(timeStr, "\n")] = 0;
    
    fprintf(g->logFile, "[%s] INVALID GUESS: %s entered '%s'\n\n", 
            timeStr, g->names[player], guess);
    fflush(g->logFile);
}

void logRoundEnd(GameState *g, int wordComplete) {
    fprintf(g->logFile, "----------------------------------------\n");
    fprintf(g->logFile, "ROUND %d END\n", g->round);
    fprintf(g->logFile, "----------------------------------------\n");
    
    if (wordComplete) {
        fprintf(g->logFile, "Status: Word COMPLETED\n");
        fprintf(g->logFile, "Answer: %s\n", g->answerWord);
    } else {
        fprintf(g->logFile, "Status: Round ended\n");
        fprintf(g->logFile, "Final board: %s\n", g->answerSpace);
    }
    
    fprintf(g->logFile, "\nRound Scores:\n");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        fprintf(g->logFile, "  %s: %d points (Lives: %d)\n", 
                g->names[i], g->scores[i], g->lives[i]);
    }
    fprintf(g->logFile, "\n");
    fflush(g->logFile);
}

void logGameEnd(GameState *g) {
    time_t now = time(NULL);
    char *timeStr = ctime(&now);
    timeStr[strcspn(timeStr, "\n")] = 0;
    
    fprintf(g->logFile, "========================================\n");
    fprintf(g->logFile, "         GAME ENDED\n");
    fprintf(g->logFile, "========================================\n");
    fprintf(g->logFile, "Game Ended: %s\n", timeStr);
    fprintf(g->logFile, "Total Rounds Played: %d\n\n", g->round);
    
    fprintf(g->logFile, "FINAL SCORES:\n");
    
    int maxScore = g->scores[0];
    int winner = 0;
    int isDraw = 0;
    
    for (int i = 1; i < MAX_PLAYERS; i++) {
        if (g->scores[i] > maxScore) {
            maxScore = g->scores[i];
            winner = i;
            isDraw = 0;
        } else if (g->scores[i] == maxScore && i != winner) {
            isDraw = 1;
        }
    }
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        fprintf(g->logFile, "  %d. %s: %d points\n", 
                i + 1, g->names[i], g->scores[i]);
    }
    
    fprintf(g->logFile, "\n");
    if (isDraw) {
        fprintf(g->logFile, "Result: DRAW!\n");
    } else {
        fprintf(g->logFile, "WINNER: %s with %d points!\n", g->names[winner], maxScore);
    }
    fprintf(g->logFile, "========================================\n");
    fflush(g->logFile);
}

void saveScores(GameState *g) {
    FILE *scoreFile = fopen(SCORE_FILE, "w");
    if (!scoreFile) {
        perror("Failed to open scores file");
        return;
    }
    
    time_t now = time(NULL);
    char *timeStr = ctime(&now);
    timeStr[strcspn(timeStr, "\n")] = 0;
    
    fprintf(scoreFile, "========================================\n");
    fprintf(scoreFile, "   WORD GUESSING GAME - FINAL SCORES\n");
    fprintf(scoreFile, "========================================\n");
    fprintf(scoreFile, "Date: %s\n", timeStr);
    fprintf(scoreFile, "Rounds Played: %d\n\n", g->round);
    
    int maxScore = g->scores[0];
    int winner = 0;
    int isDraw = 0;
    
    for (int i = 1; i < MAX_PLAYERS; i++) {
        if (g->scores[i] > maxScore) {
            maxScore = g->scores[i];
            winner = i;
            isDraw = 0;
        } else if (g->scores[i] == maxScore && i != winner) {
            isDraw = 1;
        }
    }
    
    fprintf(scoreFile, "FINAL STANDINGS:\n");
    fprintf(scoreFile, "----------------------------------------\n");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        fprintf(scoreFile, "%d. %-20s %d points\n", 
                i + 1, g->names[i], g->scores[i]);
    }
    
    fprintf(scoreFile, "\n");
    if (isDraw) {
        fprintf(scoreFile, "Result: DRAW - Multiple players tied!\n");
    } else {
        fprintf(scoreFile, "🏆 WINNER: %s with %d points!\n", g->names[winner], maxScore);
    }
    fprintf(scoreFile, "========================================\n");
    
    fclose(scoreFile);
    printf("Scores saved to %s\n", SCORE_FILE);
}

void closeLog(GameState *g) {
    if (g->logFile) {
        fclose(g->logFile);
    }
}

// ========================================
// FILE & NETWORK UTILITIES - YUNIE
// ========================================
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

// ========================================
// GAME LOGIC - NUHA
// ========================================
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

void sendGameState(GameState *g) {
    char msg[256];
    for (int i = 0; i < MAX_PLAYERS; i++) {
        sprintf(msg, "STATE:R%d|L%d|S%d", g->round + 1, g->lives[i], g->scores[i]);
        sendToClient(g, i, msg);
    }
}

int applyLetterGuess(GameState *g, int player) {
    int correct = 0;
    char letter = tolower(g->guessLetter[player]);
    
    for (int i = 0; i < strlen(g->answerWord); i++) {
        if (letter == tolower(g->answerWord[i])) {
            g->answerSpace[i] = g->answerWord[i];
            correct = 1;
        }
    }
    if (correct)
        sendBoard(g);
    return correct;
}

int checkWordGuess(GameState *g, int player) {
    char playerWord[WORD_LEN];
    strcpy(playerWord, g->guessWord[player]);
    
    for (int i = 0; playerWord[i]; i++) {
        playerWord[i] = tolower(playerWord[i]);
    }
    
    char correctWord[WORD_LEN];
    strcpy(correctWord, g->answerWord);
    for (int i = 0; correctWord[i]; i++) {
        correctWord[i] = tolower(correctWord[i]);
    }
    
    return strcmp(playerWord, correctWord) == 0;
}

int wordIsComplete(GameState *g) {
    for (int i = 0; i < strlen(g->answerWord); i++)
        if (g->answerSpace[i] == '_')
            return 0;
    return 1;
}

void sendPrompt(GameState *g, int idx) { sendToClient(g, idx, "PROMPT"); }
void sendWait(GameState *g, int idx)   { sendToClient(g, idx, "WAIT"); }
void sendInvalid(GameState *g, int idx){ sendToClient(g, idx, "INVALID"); }
void sendCorrect(GameState *g, int idx){ sendToClient(g, idx, "CORRECT"); }
void sendWrong(GameState *g, int idx)  { sendToClient(g, idx, "WRONG"); }
void sendEliminated(GameState *g, int idx) { sendToClient(g, idx, "ELIMINATED"); }

void startNewRound(GameState *g) {
    chooseRandomWord(g);
    initRound(g);
    logRoundStart(g);
    sendBoard(g);
    sendGameState(g);

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

// ========================================
// CLIENT HANDLER - QAI
// ========================================
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

        if (g->lives[me] <= 0) {
            g->currentPlayer = (g->currentPlayer + 1) % MAX_PLAYERS;
            pthread_cond_broadcast(&g->turnCond);
            pthread_mutex_unlock(&g->mutex);
            continue;
        }

        sendPrompt(g, me);
        pthread_mutex_unlock(&g->mutex);

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
            g->scores[me] -= 1;
            logTimeout(g, me);
            sendGameState(g);
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

        pthread_mutex_lock(&g->mutex);

        if (strncmp(buffer, "LETTER:", 7) == 0) {
            g->guessLetter[me] = buffer[7];

            if (!isalpha(g->guessLetter[me])) {
                logInvalidGuess(g, me, buffer + 7);
                sendInvalid(g, me);
                pthread_mutex_unlock(&g->mutex);
                continue;
            }

            int correct = applyLetterGuess(g, me);
            
            if (correct) {
                sendCorrect(g, me);
                g->scores[me] += 1;
            } else {
                sendWrong(g, me);
                g->lives[me] -= 1;
            }
            
            logLetterGuess(g, me, g->guessLetter[me], correct);
            sendGameState(g);

            if (wordIsComplete(g)) {
                logRoundEnd(g, 1);
                g->round++;
                
                sendReveal(g);

                if (g->round >= MAX_ROUNDS) {
                    logGameEnd(g);
                    saveScores(g);
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
        else if (strncmp(buffer, "WORD:", 5) == 0) {
            strncpy(g->guessWord[me], buffer + 5, WORD_LEN - 1);
            g->guessWord[me][WORD_LEN - 1] = '\0';
            g->guessWord[me][strcspn(g->guessWord[me], "\n")] = 0;

            int correct = checkWordGuess(g, me);
            
            if (correct) {
                g->scores[me] += 3;
                strcpy(g->answerSpace, g->answerWord);
                sendBoard(g);
                sendCorrect(g, me);
                logWordGuess(g, me, g->guessWord[me], 1);
                sendGameState(g);
                
                logRoundEnd(g, 1);
                g->round++;
                
                sendReveal(g);

                if (g->round >= MAX_ROUNDS) {
                    logGameEnd(g);
                    saveScores(g);
                    g->gameOver = 1;
                    sendEnd(g);
                    pthread_cond_broadcast(&g->turnCond);
                    pthread_mutex_unlock(&g->mutex);
                    break;
                }

                startNewRound(g);
                pthread_mutex_unlock(&g->mutex);
                continue;
            } else {
                g->lives[me] = 0;
                sendEliminated(g, me);
                logWordGuess(g, me, g->guessWord[me], 0);
                sendGameState(g);
            }

            g->currentPlayer = (g->currentPlayer + 1) % MAX_PLAYERS;
            pthread_cond_broadcast(&g->turnCond);
            pthread_mutex_unlock(&g->mutex);
        }
        else {
            pthread_mutex_unlock(&g->mutex);
        }
    }

    close(g->clientSockets[me]);
    exit(0);
}

// ========================================
// MAIN FUNCTION - QAI & YUNIE
// ========================================
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

    for (int i = 0; i < MAX_PLAYERS; i++) {
        g->scores[i] = 0;
        g->lives[i] = 3;
    }

    loadWords("words.txt");
    srand(time(NULL));
    
    initLog(g);

    for (int i = 0; i < MAX_PLAYERS; i++) {
        printf("Waiting for player %d...\n", i + 1);
        g->clientSockets[i] = accept(serverFd, (struct sockaddr *)&address, &addrlen);
        char buf[256] = {0};
        read(g->clientSockets[i], buf, sizeof(buf));
        if (strncmp(buf, "NAME:", 5) == 0)
            strncpy(g->names[i], buf + 5, NAME_SIZE - 1);

        printf("Player %d: %s\n", i + 1, g->names[i]);
    }

    logPlayers(g);

    printf("\nGame starting!\n");
    chooseRandomWord(g);
    initRound(g);
    logRoundStart(g);
    sendBoard(g);
    sendGameState(g);

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
    
    closeLog(g);
    return 0;
}
