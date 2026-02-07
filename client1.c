#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <sys/select.h>

#define PORT 8080
#define ANSWER_SIZE 6
#define NAME_SIZE 50
#define WORD_LEN 8

typedef struct {
    char answer_space[ANSWER_SIZE];
    char my_name[NAME_SIZE];
    int round;
    int lives;
    int score;
} ClientState;

// ---- Line-based receive (TCP safe) ----
int recvLine(int sock, char *buf, int max) {
    int i = 0;
    char c;
    while (i < max - 1) {
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) return n;
        if (c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

void displayGameState(ClientState *s) {
    printf("\n╔════════════════════════════════════════╗\n");
    printf("  ║        WORD GUESSING GAME              ║\n");
    printf("  ╠════════════════════════════════════════╣\n");
    printf("  ║ Player: %-30s                          ║\n", s->my_name);
    printf("  ║ Round:  %-30d                          ║\n", s->round);
    printf("  ║ Lives:  %-30d                          ║\n", s->lives);
    printf("  ║ Score:  %-30d                          ║\n", s->score);
    printf("  ╠════════════════════════════════════════╣\n");
    printf("  ║ Word:   ");
    for (int i = 0; i < strlen(s->answer_space); i++)
        printf("%c ", s->answer_space[i]);
    printf("%-*s║\n", (int)(24 - strlen(s->answer_space) * 2), "");
    printf("  ╚════════════════════════════════════════╝\n");
}

void clear_screen() {
    printf("\033[2J\033[H");
}

void displayMenu() {
    printf("\n┌────────────────────────────────────────────────────────────────┐\n");
    printf("  │  Choose your move:                                             │\n");
    printf("  │  1. Guess a LETTER (+1 Mark if correct, -1 Life if wrong)      │\n");
    printf("  │  2. Guess the WORD (+3 Marks if correct, Elimination if wrong) │\n");
    printf("  │                                                                │\n");
    printf("  │ WARNING: Timeout/No Input (10s timer expires) = -1 Mark!      │\n");
    printf("  └────────────────────────────────────────────────────────────────┘\n");
    printf("Your choice (1 or 2): ");
    fflush(stdout);
}

int main() {
    int sock = 0;
    struct sockaddr_in serv_addr;
    ClientState state;
    char buffer[256] = {0};

    // Initialize state
    state.round = 1;
    state.lives = 3;
    state.score = 0;

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("Socket creation error\n");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        printf("Invalid address\n");
        return 1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("Connection failed\n");
        return 1;
    }

    printf("╔════════════════════════════════════════╗\n");
    printf("║   Connected to Word Guessing Server    ║\n");
    printf("╚════════════════════════════════════════╝\n\n");

    // Register name
    printf("Enter your name: ");
    fflush(stdout);
    fgets(state.my_name, NAME_SIZE, stdin);
    state.my_name[strcspn(state.my_name, "\n")] = 0;

    char name_msg[256];
    sprintf(name_msg, "NAME:%s", state.my_name);
    send(sock, name_msg, strlen(name_msg), 0);

    printf("\n  Waiting for game to start...\n");

    // Receive initial board
    recvLine(sock, buffer, sizeof(buffer));
    if (strncmp(buffer, "BOARD:", 6) == 0) {
        strcpy(state.answer_space, buffer + 6);
        clear_screen();
        displayGameState(&state);
    }

    int game_active = 1;
    int is_eliminated = 0;

    while (game_active) {
        memset(buffer, 0, sizeof(buffer));
        int valread = recvLine(sock, buffer, sizeof(buffer));
        if (valread <= 0) {
            printf("\n  Disconnected from server\n");
            break;
        }

        // Handle PROMPT - your turn
        if (strcmp(buffer, "PROMPT") == 0) {
            // ✅ FIXED: If eliminated, just wait for next message instead of continue loop
            if (is_eliminated) {
                printf("\n  💀 You are eliminated. Skipping turn...\n");
                continue;  // This is OK - will go back to recvLine() and get next message
            }

            printf("\n\n");
            printf("═══════════════════════════════════════\n");
            printf("║          YOUR TURN! (10 seconds)    ║\n");
            printf("═══════════════════════════════════════\n");
            
            // ✅ FIXED: Loop for valid input INSIDE the PROMPT handler
            int valid_move = 0;
            while (!valid_move) {
                displayMenu();

                fd_set readfds;
                struct timeval tv;
                FD_ZERO(&readfds);
                FD_SET(STDIN_FILENO, &readfds);
                tv.tv_sec = 10;
                tv.tv_usec = 0;

                int ready = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);

                if (ready == 0) {
                    printf("\n\n   *** TIME'S UP! (-1 point penalty) ***\n");
                    fflush(stdout);
                    valid_move = 1;  // Exit loop, turn is over
                    break;
                }

                int choice;
                if (scanf("%d", &choice) != 1) {
                    while (getchar() != '\n');
                    printf("\n❌ Invalid input! Try again.\n");
                    continue;  // Re-prompt inside this loop
                }
                while (getchar() != '\n');

                if (choice == 1) {
                    // Letter guess
                    printf("Enter a letter: ");
                    fflush(stdout);
                    
                    char letter;
                    if (scanf(" %c", &letter) != 1) {
                        while (getchar() != '\n');
                        printf("\n❌ Invalid input! Try again.\n");
                        continue;  // Re-prompt
                    }
                    while (getchar() != '\n');

                    char move_msg[20];
                    sprintf(move_msg, "LETTER:%c", letter);
                    send(sock, move_msg, strlen(move_msg), 0);
                    valid_move = 1;

                } else if (choice == 2) {
                    // Word guess
                    printf("Enter the complete word: ");
                    fflush(stdout);
                    
                    char word[WORD_LEN];
                    if (fgets(word, WORD_LEN, stdin) == NULL) {
                        printf("\n❌ Invalid input! Try again.\n");
                        continue;  // Re-prompt
                    }
                    word[strcspn(word, "\n")] = 0;

                    char move_msg[20];
                    sprintf(move_msg, "WORD:%s", word);
                    send(sock, move_msg, strlen(move_msg), 0);
                    valid_move = 1;

                } else {
                    printf("\n❌ Invalid choice! Please enter 1 or 2.\n");
                    // Loop will re-prompt
                }
            }
        }
        // Handle WAIT - opponent's turn
        else if (strcmp(buffer, "WAIT") == 0) {
            if (!is_eliminated) {
                printf("\n Waiting for opponent's move...\n");
                fflush(stdout);
            }
        }
        // Handle BOARD update
        else if (strncmp(buffer, "BOARD:", 6) == 0) {
            strcpy(state.answer_space, buffer + 6);
            clear_screen();
            displayGameState(&state);
        }
        // Handle STATE update (Round|Lives|Score)
        else if (strncmp(buffer, "STATE:", 6) == 0) {
            sscanf(buffer + 6, "R%d|L%d|S%d", &state.round, &state.lives, &state.score);
            displayGameState(&state);
            
            // ✅ Check if we just got eliminated via STATE update
            if (state.lives <= 0 && !is_eliminated) {
                is_eliminated = 1;
            }
        }
        // Handle INVALID guess
        else if (strcmp(buffer, "INVALID") == 0) {
            printf("\n❌ *** Invalid move! That's not a valid letter ***\n");
        }
        // Handle CORRECT guess
        else if (strcmp(buffer, "CORRECT") == 0) {
            clear_screen();
            printf("\n✅ *** CORRECT! Good guess! ***\n");
        }
        // Handle WRONG guess
        else if (strcmp(buffer, "WRONG") == 0) {
            clear_screen();
            printf("\n❌ *** WRONG! Try again... ***\n");
        }
        // Handle ELIMINATED (wrong word guess)
        else if (strcmp(buffer, "ELIMINATED") == 0) {
            clear_screen();
            printf("\n");
            printf("╔═══════════════════════════════════════╗\n");
            printf("║                                       ║\n");
            printf("║        YOU HAVE BEEN ELIMINATED!      ║\n");
            printf("║                                       ║\n");
            printf("║   Wrong word guess - Game Over!       ║\n");
            printf("║                                       ║\n");
            printf("╚═══════════════════════════════════════╝\n");
            is_eliminated = 1;
            state.lives = 0;
        }
        // Handle REVEAL (round end)
        else if (strncmp(buffer, "REVEAL:", 7) == 0) {
            clear_screen();
            printf("\n");
            printf("╔═══════════════════════════════════════╗\n");
            printf("║                                       ║\n");
            printf("║             ROUND %d ENDED            ║\n", state.round);
            printf("║                                       ║\n");
            printf("║    THE ANSWER WAS: %-18s              ║\n", buffer + 7);
            printf("║                                       ║\n");
            printf("╚═══════════════════════════════════════╝\n");
        }
        // Handle END (game over)
        else if (strcmp(buffer, "END") == 0) {
            printf("\n");
            printf("╔═══════════════════════════════════════╗\n");
            printf("║                                       ║\n");
            printf("║            GAME ENDED!                ║\n");
            printf("║                                       ║\n");
            printf("║         Final Score: %-19d          ║\n", state.score);
            printf("║                                       ║\n");
            printf("║     Check scores.txt for rankings!    ║\n");
            printf("║                                       ║\n");
            printf("╚═══════════════════════════════════════╝\n");
            game_active = 0;
        }
    }

    close(sock);
    return 0;
}