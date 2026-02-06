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

void displayAnswerSpace(ClientState *s) {
    printf("\n==============================\n");
    printf("Player: %s\n", s->my_name);
    printf("Word: ");
    for (int i = 0; i < strlen(s->answer_space); i++)
        printf("%c ", s->answer_space[i]);
    printf("\n==============================\n");
}

void clear_screen() {
    printf("\033[2J\033[H");
}

int main() {
    int sock = 0;
    struct sockaddr_in serv_addr;
    ClientState state;
    char buffer[256] = {0};

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

    printf("Connected to server\n");
    printf("==================================\n");

    // Register name
    printf("Enter your name: ");
    fflush(stdout);
    fgets(state.my_name, NAME_SIZE, stdin);
    state.my_name[strcspn(state.my_name, "\n")] = 0;

    char name_msg[256];
    sprintf(name_msg, "NAME:%s\n", state.my_name);
    send(sock, name_msg, strlen(name_msg), 0);

    printf("\nWaiting for game to start...\n");

    // Receive initial board
    recvLine(sock, buffer, sizeof(buffer));
    if (strncmp(buffer, "BOARD:", 6) == 0) {
        strcpy(state.answer_space, buffer + 6);
        clear_screen();
        displayAnswerSpace(&state);
    }

    int game_active = 1;
    state.round = 1;

    while (game_active) {
        memset(buffer, 0, sizeof(buffer));
        int valread = recvLine(sock, buffer, sizeof(buffer));
        if (valread <= 0) {
            printf("Disconnected from server\n");
            break;
        }

        if (strcmp(buffer, "PROMPT") == 0) {
            printf("\n>> YOUR TURN! (10 seconds) <<\n");
            printf("Input next letter: ");
            fflush(stdout);

            fd_set readfds;
            struct timeval tv;
            FD_ZERO(&readfds);
            FD_SET(STDIN_FILENO, &readfds);
            tv.tv_sec = 10;
            tv.tv_usec = 0;

            int ready = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);

            if (ready == 0) {
                printf("\n*** TIME'S UP! ***\n");
                fflush(stdout);
                continue;
            }

            char letter;
            if (scanf(" %c", &letter) != 1) {
                while (getchar() != '\n');
                printf("\nInvalid input!\n");
                continue;
            }
            while (getchar() != '\n');

            char move_msg[20];
            sprintf(move_msg, "MOVE:%c\n", letter);
            send(sock, move_msg, strlen(move_msg), 0);
        }
        else if (strcmp(buffer, "WAIT") == 0) {
            printf("\n>> Waiting for opponent's move... <<\n");
            fflush(stdout);
        }
        else if (strncmp(buffer, "BOARD:", 6) == 0) {
            strcpy(state.answer_space, buffer + 6);
            
            displayAnswerSpace(&state);
        }
        else if (strcmp(buffer, "INVALID") == 0) {
            printf("\n*** Invalid move! That's not a letter\n");
        }
        else if (strcmp(buffer, "CORRECT") == 0) {
            clear_screen();
            printf("\nGood Guess!\n");
        }
        else if (strcmp(buffer, "WRONG") == 0) {
            clear_screen();
            printf("\nWrong guess..\n");
        }
        else if (strncmp(buffer, "REVEAL:", 7) == 0) {
            clear_screen();
            printf("=========================================\n");
            printf("         ROUND %d ENDED\n", state.round);
            printf("      THE ANSWER WAS: %s\n", buffer + 7);
            printf("=========================================\n");
            state.round++;
        }
        else if (strcmp(buffer, "END") == 0) {
            printf("\n=========================================\n");
            printf("              GAME ENDED!\n");
            printf("=========================================\n");
            game_active = 0;
        }
    }

    close(sock);
    return 0;
}




/*
 else if (strncmp(buffer, "COMPLETE0:", 7) == 0) {
            char winner_name[NAME_SIZE];
            strcpy(winner_name, buffer + 7);
            printf("\n");
            printf("=========================================\n");
            if (strcmp(winner_name, state.my_name) == 0) {
                printf(">> CONGRATULATIONS! YOU WIN!\n");
            } else {
                printf("%s wins! Better luck next time.\n", winner_name);
            }
            printf("=========================================\n");
            game_active = 0;
        } 
        else if (strncmp(buffer, "DRAW", 4) == 0) {
            printf("\n");
            printf("=========================================\n");
            printf("        GAME ENDED IN A DRAW!\n");
            printf("=========================================\n");
            game_active = 0;
        }

*/