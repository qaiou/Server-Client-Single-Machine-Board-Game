#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <sys/select.h>

#define PORT 8080
#define ANSWER_SIZE 5
#define NAME_SIZE 50
#define WORD_LEN 8

#define BOARD_SIZE 9

typedef struct {
    char answer_space[ANSWER_SIZE];
    char my_name[NAME_SIZE];
    char guess_letter;           // player's letter guess
    char guess_word[WORD_LEN];    // player's word guess (if player decides to guess a whole word)
    int lives;                   // player's lives
} ClientState;

void display_label_board() {
    
}

void displayAnswerSpace(ClientState *s) {
    printf("\n==============================\n");
    printf("Player: %s\n", s->my_name);
    printf("Word: ");
    for (int i = 0; i < WORD_LEN; i++)
        printf("%c ", s->answer_space[i]);
    printf("\n==============================\n");
}

void clear_screen() {
    printf("\033[2J\033[H");
}

int main() {
    int sock = 0; //to server
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
    fflush(stdout); //to make sure input is printed
    fgets(state.my_name, NAME_SIZE, stdin); //input is put in var my_name
    state.my_name[strcspn(state.my_name, "\n")] = 0; //remove newline

    char name_msg[256];
    sprintf(name_msg, "NAME:%s", state.my_name);
    send(sock, name_msg, strlen(name_msg), 0);

    printf("\nWaiting for game to start...\n");

    // Receive initial board
    memset(buffer, 0, 256);
    read(sock, buffer, 256);
    if (strncmp(buffer, "BOARD:", 6) == 0) {
        memcpy(state.answer_space, buffer + 6, BOARD_SIZE);
        clear_screen();
        displayAnswerSpace(&state);
    }

    int game_active = 1;
    while (game_active) {
        memset(buffer, 0, 256);
        int valread = read(sock, buffer, 256);

        if (valread <= 0) {
            printf("Disconnected from server\n");
            break;
        }

        if (strncmp(buffer, "PROMPT", 6) == 0) {
            printf("\n>> YOUR TURN! (10 seconds) <<\n");
            printf("Input next grid number (1-9): ");
            fflush(stdout);

            fd_set readfds;
            struct timeval tv;
            FD_ZERO(&readfds);
            FD_SET(STDIN_FILENO, &readfds);
            tv.tv_sec = 10;
            tv.tv_usec = 0;

            int ready = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);

            if (ready == 0) {
                // timeout. server will confirm too
                printf("\n*** TIME'S UP! ***\n");
                fflush(stdout);
                continue;
            }

            int move;
            if (scanf("%d", &move) != 1) {
                while (getchar() != '\n');
                printf("\nInvalid input!\n");
                continue;
            }
            while (getchar() != '\n');

            char move_msg[20];
            sprintf(move_msg, "MOVE:%d", move);
            send(sock, move_msg, strlen(move_msg), 0);
        } 
        else if (strncmp(buffer, "WAIT", 4) == 0) {
            printf("\n>> Waiting for opponent's move... <<\n");
            fflush(stdout);
        } 
        else if (strncmp(buffer, "TIMEOUT", 7) == 0) {
            printf("\n*** Time's up! You lost your turn. ***\n");
            fflush(stdout);
        }
        else if (strncmp(buffer, "BOARD:", 6) == 0) {
            memcpy(state.answer_space, buffer + 6, BOARD_SIZE);
            clear_screen();
            displayAnswerSpace(&state);
        } 
        else if (strncmp(buffer, "INVALID", 7) == 0) {
            printf("\n*** Invalid move! That position is already taken.\n");
        } 
        else if (strncmp(buffer, "WINNER:", 7) == 0) {
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
    }

    close(sock);
    return 0;
}