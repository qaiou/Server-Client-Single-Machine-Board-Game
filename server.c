#include <stdio.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>
#include <ctype.h>

#define PORT 8080
#define BOARD_SIZE 9
#define NAME_SIZE 50



typedef struct {
    pthread_mutex_t mutex;
    char board[BOARD_SIZE];          // 'X', 'O', or ' ' (space)
    int client_sockets[2];
    char symbols[2];                 // symbols[0] for player 1, symbols[1] for player 2
    char names[2][NAME_SIZE];
    int current_player;              // 0 or 1
    int game_over;                   // 0 or 1
} GameState;

void init_game(GameState *game) {
    for (int i = 0; i < BOARD_SIZE; i++){
        game->board[i] = ' ';
    }
    game->current_player = 0;
    game->game_over = 0;
}

void log_move(char *name, int position, char symbol) {
    FILE *fp = fopen("game_log.txt", "a");
    if (fp) {
        time_t now = time(NULL);
        char *time_str = ctime(&now);
        time_str[strlen(time_str)-1] = '\0';
        fprintf(fp, "[%s] %s (%c) moved to position %d\n", time_str, name, symbol, position);
        fclose(fp);
    }
}

void log_winner(char *name, char symbol) {
    FILE *fp = fopen("winners.txt", "a");
    if (fp) {
        time_t now = time(NULL);
        char *time_str = ctime(&now);
        time_str[strlen(time_str) - 1] = '\0';
        fprintf(fp, "[%s] %s (%c) won the game!\n", time_str, name, symbol);
        fclose(fp);
    }
}

void log_draw() {
    FILE *fp = fopen("winners.txt", "a");
    if (fp) {
        time_t now = time(NULL);
        char *time_str = ctime(&now);
        time_str[strlen(time_str) - 1] = '\0';
        fprintf(fp, "[%s] Game ended in a draw\n", time_str);
        fclose(fp);
    }
}


int check_winner(GameState *game) {
    int wins[8][3] = {
        {0,1,2}, {3,4,5}, {6,7,8},
        {0,3,6}, {1,4,7}, {2,5,8},
        {0,4,8}, {2,4,6}
    };
    for (int i = 0; i < 8; i++) {
        int a = wins[i][0], b = wins[i][1], c = wins[i][2];
        if (game->board[a] != ' ' &&
            game->board[a] == game->board[b] &&
            game->board[b] == game->board[c]) {
            char sym = game->board[a];
            return (game->symbols[0] == sym) ? 0 : 1;
        }
    }
    return -1;
}

int check_draw(GameState *game) {
    for (int i = 0; i < BOARD_SIZE; i++) {
        if (game->board[i] == ' ') {
            return 0;
        }
    }
    return 1;
}

void send_board_update(GameState *game, int client_idx) {
    char board_str[BOARD_SIZE + 1];
    memcpy(board_str, game->board, BOARD_SIZE);
    board_str[BOARD_SIZE] = '\0';
    char buffer[256];
    sprintf(buffer, "BOARD:%s", board_str);
    send(game->client_sockets[client_idx], buffer, strlen(buffer), 0);
}

void broadcast_board(GameState *game) {
    for (int i = 0; i < 2; i++) {
        send_board_update(game, i);
    }
}

void send_prompt_message(GameState *game, int client_idx) {
    char msg[] = "PROMPT";
    send(game->client_sockets[client_idx], msg, strlen(msg), 0);
}

void send_wait_message(GameState *game, int client_idx) {
    char msg[] = "WAIT";
    send(game->client_sockets[client_idx], msg, strlen(msg), 0);
}

int main() {
    int server_fd;
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);
    GameState *game = NULL;
    int shm_fd;
    char buffer[256];

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 2) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", PORT);
    printf("Waiting for players to connect...\n\n");

    char first_symbol = '\0';

    shm_fd = shm_open("/tictactoe_shm", O_CREAT | O_RDWR, 0666);
    ftruncate(shm_fd, sizeof(GameState));

    game = (GameState *)mmap(NULL, sizeof(GameState), PROT_READ | PROT_WRITE, MAP_SHARED, -1, 0);
    
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&game->mutex, &attr);

    for (int i = 0; i < 2; i++) {
        printf("Waiting for player %d to connect...\n", i + 1);
        if ((game->client_sockets[i] = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) <0 ) {
            perror("Accept failed");
            exit(EXIT_FAILURE);
        }

        printf("Player %d connected!\n", i + 1);

        //receive player name
        char buffer[256] = {0}; // temporary simpan the value sent from client
        read(game->client_sockets[i], buffer,256);
        if (strncmp(buffer, "NAME:", 5) == 0) {
            strncpy(game->names[i], buffer + 5, NAME_SIZE - 1);
            game->names[i][NAME_SIZE - 1] = '\0';
            printf("Player %d name: %s\n", i + 1, game->names[i]);
        }

        //handle symbol selection
        if (i == 0) {
            //first player choose
            char choice_msg[] = "CHOOSE";
            send(game->client_sockets[i], choice_msg, strlen(choice_msg), 0);

            memset(buffer, 0, 256);
            read(game->client_sockets[i], buffer, 256);
            if (strncmp(buffer, "SYMBOL:", 7) == 0) {
                first_symbol = buffer[7];
                game->symbols[0] = first_symbol;
                printf("Player 1 (%s) chose: %c\n", game->names[0], first_symbol);

                char confirm[50];
                sprintf(confirm, "ASSIGNED:%c", first_symbol);
                send(game->client_sockets[i], confirm, strlen(confirm), 0);
            }
        } else {
            //second player gets the other symbol
            char second_symbol = (first_symbol == 'X') ? 'O' : 'X';
            game->symbols[1] = second_symbol;

            char assign_msg[50];
            sprintf(assign_msg, "ASSIGNED:%c", second_symbol);
            send(game->client_sockets[i], assign_msg, strlen(assign_msg), 0);
            printf("Player 2 (%s) assigned: %c\n", game->names[1], second_symbol);
        }
    }

    printf("\nGame starting!\n");
    printf("%s (%c) vs %s (%c)\n\n", game->names[0], game->symbols[0], game->names[1], game->symbols[1]);

    init_game(game);
    broadcast_board(game);

    //send initial turn messages
    send_prompt_message(game, 0);
    send_wait_message(game, 1);

    while (!game->game_over) {
        int current = game->current_player;

        char buffer[256] = {0};
        int valread = read(game->client_sockets[current], buffer, 256);

        if (valread <= 0) {
            printf("Player %s disconnected\n", game->names[current]);
            break;
        }

        if (strncmp(buffer, "MOVE:", 5) == 0) {
            int position = atoi(buffer + 5) - 1;
            
            if (position >= 0 && position < BOARD_SIZE && game->board[position] == ' ') {

                game->board[position] = game->symbols[current];
                log_move(game->names[current], position, game->symbols[current]);
                printf("%s (%c) moved to position %d\n",
                       game->names[current], game->symbols[current], position + 1);

                //Broadcast updated board to both client first
                broadcast_board(&game);
                usleep(100000); //small delay to ensure board is received

                int winner = check_winner(&game);
                if (winner != -1) {
                    char win_msg[100];
                    sprintf(win_msg, "WINNER:%s", game->names[winner]);
                    for (int i = 0; i < 2; i++) {
                        send(game->client_sockets[i], win_msg, strlen(win_msg), 0);
                    }
                    log_winner(game->names[winner], game->symbols[winner]);
                    game->game_over = 1;
                    printf("\n%s (%c) wins!\n", game->names[winner], game->symbols[winner]);
                } else if (check_draw(&game)) {
                    char draw_msg[] = "DRAW";
                    for (int i = 0; i < 2; i++) {
                        send(game->client_sockets[i], draw_msg, strlen(draw_msg), 0);
                    }
                    log_draw();
                    game->game_over = 1;
                    printf("\nGame is a draw!\n");
                } else {
                    //switch to next player
                    game->current_player = 1 - game->current_player;

                    //send turn messages for next round
                    send_prompt_message(game, game->current_player);
                    send_wait_message(game, 1 - game->current_player);
                }
            } else {
                char invalid[] = "INVALID";
                send(game->client_sockets[current], invalid, strlen(invalid), 0);
                //resend prompt after invalid move
                send_prompt_message(game, current);
            }
        }
    }

    for (int i = 0; i < 2; i++) {
        close(game->client_sockets[i]);
    }
    munmap(game, sizeof(GameState));
    close(server_fd);
    
    return 0;
}