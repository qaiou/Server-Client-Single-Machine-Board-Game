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


//--------shared memory------------
typedef struct {
    pthread_mutex_t mutex;
    char answer_space[ANSWER_SIZE]; // 5 answer spaces. initially is all '_'        
    int client_sockets[3];          // 3 players (or more)
    char names[3][NAME_SIZE];       // players' name
    char answer_word[WORD_LEN];      // Max 5 letters for each word
    char guess_letter[3];           // player's letter guess
    char guess_word[3][WORD_LEN];    // player's word guess (if player decides to guess a whole word)
    int lives[3];                   // players' lives

    int current_player;              
    int game_over;                  // 0 or 1
} GameState;


char words[MAX_WORDS][WORD_LEN];
int word_count = 0;

void load_words(const char *filename) {   //done
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        perror("Failed to open words file");
        exit(1);
    }

    while (fgets(words[word_count], WORD_LEN, fp) && word_count < MAX_WORDS) {
        words[word_count][strcspn(words[word_count], "\n")] = 0;
        word_count++;
    }

    fclose(fp);
}

void init_game(GameState *game) {
    for (int i = 0; i < ANSWER_SIZE; i++){
        game->answer_space[i] = '_';
    }
    game->current_player = 0;
    game->game_over = 0;
}

int isCorrect(GameState *game, int current) {
    for (int i = 0; i < WORD_LEN; i++){
        if(game->guess_letter[current] == game->answer_word[i]){
            game->answer_space[i] = game->guess_letter[current];
            return 1;
        }
    }
    return 0;
}

int check_winner(GameState *game) {
}

int check_draw(GameState *game) {  //if wordIsComplete() = false && all players' lives = 0
}

void updateAnswerSpaces(GameState *game, int client_idx) {
    char answer_str[ANSWER_SIZE + 1];
    memcpy(answer_str, game->answer_space, ANSWER_SIZE);
    answer_str[ANSWER_SIZE] = '\0';
    char buffer[256];
    sprintf(buffer, "BOARD:%s", answer_str);
    send(game->client_sockets[client_idx], buffer, strlen(buffer), 0);
}

void broadcast_board(GameState *game) {
    for (int i = 0; i < 3; i++) {
        updateAnswerSpaces(game, i);
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

int wordIsComplete(GameState *game) {
    for (int i = 0; i < WORD_LEN; i++)
        if (game->answer_space[i] == '_')
            return 0;
    return 1;
}

void handle_timeout(GameState *game, int current) {
    char msg[] = "TIMEOUT";
    send(game->client_sockets[current], msg, strlen(msg), 0);

    game->current_player = (game->current_player + 1) % 3;

    send_wait_message(game, current);
    send_prompt_message(game, game->current_player);
    
}

void handle_client(int current, GameState *game) {
    char buffer[256];

    while (1) {

        // Only current player is allowed to wait for input
        pthread_mutex_lock(&game->mutex);
        if (game->game_over) {
            pthread_mutex_unlock(&game->mutex);
            break;
        }

        if (current != game->current_player) {
            pthread_mutex_unlock(&game->mutex);
            usleep(100000);
            continue;
        }

        send_prompt_message(game, current);
        pthread_mutex_unlock(&game->mutex);

        // ---- ROUND ROBIN WITH 10s TIME LIMIT ----
        fd_set readfds;
        struct timeval tv;

        FD_ZERO(&readfds);
        FD_SET(game->client_sockets[current], &readfds);
        tv.tv_sec = 10;
        tv.tv_usec = 0;

        int ready = select(game->client_sockets[current] + 1, &readfds, NULL, NULL, &tv);
        pthread_mutex_lock(&game->mutex);

        if (ready == 0) {
            // TIMEOUT
            if (!game->game_over && current == game->current_player) {
                printf("%s timed out!\n", game->names[current]);
                handle_timeout(game, current);
            }
            pthread_mutex_unlock(&game->mutex);
            continue;
        }

        if (ready < 0) {
            perror("select");
            pthread_mutex_unlock(&game->mutex);
            break;
        }

        pthread_mutex_unlock(&game->mutex);

        // ---- INPUT RECEIVED ----
        memset(buffer, 0, sizeof(buffer));
        int valread = read(game->client_sockets[current], buffer, sizeof(buffer));
        if (valread <= 0) {
            printf("Player %s disconnected\n", game->names[current]);
            break;
        }

        if (strncmp(buffer, "MOVE:", 5) != 0)
            continue;

        game->guess_letter[current]=buffer[5];

        //lock before modify
        pthread_mutex_lock(&game->mutex);

        if (game->game_over) {
            pthread_mutex_unlock(&game->mutex);
            break;
        }
        
        if (!isalpha(game->guess_letter[current])) {
            send(game->client_sockets[current], "INVALID", 7, 0);
            continue;
        }

        // if correct/ wrong
        if (isCorrect(game, current)){
            //log_move(game->names[current], position, game->guess_letter[current]);
        printf("%s  got (%c) correct \n", game->names[current], game->guess_letter[current]);
        }
        else{
            printf("%s  got (%c) wrong \n", game->names[current], game->guess_letter[current]);
        }

        broadcast_board(game);

        //check if word is already completed/ draw or win
        if (wordIsComplete(game)){

            //scoring

            //game_over =1
            
        }

        // if game not over yet, proceed to next player
        if (!game->game_over)
            game->current_player = (game->current_player + 1) % 3;
        
        pthread_mutex_unlock(&game->mutex);

        //update the answer spaces
        broadcast_board(game);
        
        // round finish message to client
        /*
        if (winner != -1) {
            char win_msg[100];
            sprintf(win_msg, "WINNER:%s", game->names[winner]);
            for (int i = 0; i < 2; i++)
                send(game->client_sockets[i], win_msg, strlen(win_msg), 0);
            //log_winner(game->names[winner], game->answer_space[winner]);
            break;
        }

        if (draw) {
            for (int i = 0; i < 2; i++)
                send(game->client_sockets[i], "DRAW", 4, 0);
            //log_draw();
            break;
        }*/

        // ---- ROUND ROBIN SWITCH ----
        send_wait_message(game, current);
    }

    close(game->client_sockets[current]);
    munmap(game, sizeof(GameState));
    exit(0);
}


int main() {
    signal(SIGCHLD, SIG_IGN); //prevent zombie processes
    
    int server_fd; //server file descriptor
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address); 
    GameState *game = NULL;
    int shm_fd;
    char buffer[256]; 

    // Creating socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    //server addressing
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

    //-------------create and open shared memory object-----------------------
    shm_fd = shm_open("/tictactoe_shm", O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open failed");
        exit(EXIT_FAILURE);
    }

    if(ftruncate(shm_fd, sizeof(GameState)) == -1) { 
        perror("ftruncate failed");
        shm_unlink("/tictactoe_shm");
        exit(EXIT_FAILURE);
    }

    game = (GameState *)mmap(NULL, sizeof(GameState), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    
    //-----------------initialize mutex------------------------
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&game->mutex, &attr);


    char first_symbol = '\0';
    init_game(game);

    //--------------- Load words and declare randomizer---------
    load_words("words.txt");   
    if (word_count == 0) {
        printf("No words loaded. Check words.txt\n");
        exit(1);
    }
    srand(time(NULL));

    /* 
    strcpy(game->answer_word, words[rand() % word_count]);

    printf("\nRandom word selected: %s\n", game->answer_word);
    */ //test if random word select works ^^


    //-------------Player (client) connects ---------------------
    for (int i = 0; i < 3; i++) {
        printf("Waiting for player %d to connect...\n", i + 1);

        //accept connection
        if ((game->client_sockets[i] = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) <0 ) {
            perror("Accept failed");
            exit(EXIT_FAILURE);
        }

        printf("Player %d connected!\n", i + 1);

        //receive player name
        char buffer[256] = {0}; // temporary save the value sent from client
        read(game->client_sockets[i], buffer,256);
        if (strncmp(buffer, "NAME:", 5) == 0) {
            strncpy(game->names[i], buffer + 5, NAME_SIZE - 1);
            game->names[i][NAME_SIZE - 1] = '\0';
            printf("Player %d name: %s\n", i + 1, game->names[i]);
        }
        
    }

    //Parents starts the game
    printf("\nGame starting!\n");
    printf("%s vs %s vs %s \n\n", game->names[0], game->names[1], game->names[2]);

    broadcast_board(game);

    //send initial turn messages
    send_wait_message(game, 1);
    send_wait_message(game, 2);


    //forking processes for each client
    for (int i = 0; i < 3; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            //child process handles client
            close(server_fd);
            handle_client(i, game);
        }
    }

    //wait for child processes to finish
    while(1){
        pause();
    }

    munmap(game, sizeof(GameState));
    shm_unlink("/tictactoe_shm");
    
    return 0;
}