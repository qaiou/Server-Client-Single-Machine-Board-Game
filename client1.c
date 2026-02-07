#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>


#define PORT 8080
#define ANSWER_SIZE 50
#define NAME_SIZE 50
#define WORD_LEN 20
#define MAX_RETRIES 3

typedef struct {
    char answer_space[ANSWER_SIZE];
    char my_name[NAME_SIZE];
    char current_turn_player[NAME_SIZE];
    int round;
    int lives;
    int score;
    int needs_display;
} ClientState;

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
    printf("║        WORD GUESSING GAME              ║\n");
    printf("╠════════════════════════════════════════╣\n");
    printf("║ Player: %-30s ║\n", s->my_name);
    printf("║ Round:  %-30d ║\n", s->round);
    printf("║ Lives:  %-30d ║\n", s->lives);
    printf("║ Score:  %-30d ║\n", s->score);
    printf("╠════════════════════════════════════════╣\n");
    printf("║ Word:   ");
    for (int i = 0; i < strlen(s->answer_space); i++)
        printf("%c ", s->answer_space[i]);
    printf("%-*s         ║\n", (int)(24 - strlen(s->answer_space) * 2), "");
    printf("╚════════════════════════════════════════╝\n");
}

void clear_screen() {
    printf("\033[2J\033[H");
}

void displayMenu() {
    printf("\n┌────────────────────────────────────────────────────────────────┐\n");
    printf("│  Choose your move:                                             │\n");
    printf("│  1. Guess a LETTER (+1 Mark if correct, -1 Life if wrong)      │\n");
    printf("│  2. Guess the WORD (+3 Marks if correct, ELIMINATION if wrong) │\n");
    printf("│                                                                │\n");
    printf("│  [WARNING: Timeout after 15 seconds = -1 Mark!]                │\n");
    printf("└────────────────────────────────────────────────────────────────┘\n");
    printf("Your choice (1 or 2): ");
    fflush(stdout);
}

// NEW: Simpler input with visible timer
int get_input_with_timer(char *buffer, int max_len, int timeout, const char *prompt) {
    printf("%s", prompt);
    fflush(stdout);
    
    fd_set readfds;
    struct timeval tv;
    time_t start_time = time(NULL);
    time_t current_time;
    int time_left;
    
    // Non-blocking input loop
    while (1) {
        current_time = time(NULL);
        time_left = timeout - (current_time - start_time);
        
        if (time_left <= 0) {
            printf("\n\n*** TIME'S UP! Turn forfeited. ***\n");
            fflush(stdout);
            // Clear input buffer
            int c;
            while ((c = getchar()) != '\n' && c != EOF);
            return 0;
        }
        
        // Show countdown every second
        printf("\r%s[Time left: %d sec] ", prompt, time_left);
        fflush(stdout);
        
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int ready = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
        
        if (ready > 0) {
            // Input available
            if (fgets(buffer, max_len, stdin) != NULL) {
                buffer[strcspn(buffer, "\n")] = '\0';
                printf("\n");
                return 1;
            }
        } else if (ready < 0) {
            perror("select");
            return 0;
        }
    }
}

int main() {
    int sock = 0;
    struct sockaddr_in serv_addr;
    ClientState state;
    char buffer[256] = {0};

    memset(&state, 0, sizeof(state));
    state.round = 1;
    state.lives = 3;
    state.score = 0;
    state.needs_display = 1;
    strcpy(state.current_turn_player, "Waiting...");

    // Create socket
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

    printf("Enter your name: ");
    fflush(stdout);
    fgets(state.my_name, NAME_SIZE, stdin);
    state.my_name[strcspn(state.my_name, "\n")] = 0;

    // Send name to server
    char name_msg[256];
    snprintf(name_msg, sizeof(name_msg), "NAME:%s\n", state.my_name);
    send(sock, name_msg, strlen(name_msg), 0);

    printf("\n✓ Name sent: %s\n", state.my_name);
    printf("Waiting for other players to join...\n");

    int game_active = 1;
    int is_eliminated = 0;

    while (game_active) {
        memset(buffer, 0, sizeof(buffer));
        int valread = recvLine(sock, buffer, sizeof(buffer));
        
        if (valread <= 0) {
            printf("\nDisconnected from server\n");
            break;
        }

        // Handle server messages
        if (strncmp(buffer, "TURN:", 5) == 0) {
            strcpy(state.current_turn_player, buffer + 5);
            if (strcmp(state.current_turn_player, state.my_name) != 0 && !is_eliminated) {
                clear_screen();
                displayGameState(&state);
                printf("\n*** %s's TURN... ***\n", state.current_turn_player);
                printf("Waiting for %s to make a move...\n", state.current_turn_player);
            }
            state.needs_display = 0;
            continue;
        }

        if (strcmp(buffer, "PROMPT") == 0) {
            if (is_eliminated) {
                printf("\nYou are eliminated. Spectating...\n");
                continue;
            }

            clear_screen();
            displayGameState(&state);
            
            printf("\n");
            printf("═══════════════════════════════════════\n");
            printf("║          YOUR TURN!                 ║\n");
            printf("═══════════════════════════════════════\n");

            displayMenu();
            
            int valid_choice = 0;
            int choice;
            int retry_count = 0;
            
            // Get choice with validation
            while (!valid_choice && retry_count < MAX_RETRIES) {
                if (scanf("%d", &choice) != 1) {
                    while (getchar() != '\n');
                    retry_count++;
                    printf("\nInvalid input! Please enter 1 or 2. ");
                    printf("(Attempt %d/%d)\n", retry_count, MAX_RETRIES);
                    
                    if (retry_count >= MAX_RETRIES) {
                        printf("\nToo many invalid attempts! Turn forfeited.\n");
                        break;
                    }
                    printf("\nYour choice (1 or 2): ");
                    continue;
                }
                while (getchar() != '\n');

                if (choice == 1 || choice == 2) {
                    valid_choice = 1;
                } else {
                    retry_count++;
                    printf("\nInvalid choice! Must be 1 or 2. ");
                    printf("(Attempt %d/%d)\n", retry_count, MAX_RETRIES);
                    
                    if (retry_count >= MAX_RETRIES) {
                        printf("\nToo many invalid attempts! Turn forfeited.\n");
                        break;
                    }
                    printf("\nYour choice (1 or 2): ");
                }
            }

            if (!valid_choice) {
                printf("\nTurn skipped.\n");
                send(sock, "LETTER:X\n", 9, 0);
                continue;
            }

            clear_screen();
            displayGameState(&state);
            printf("\n");
            
            if (choice == 1) {
                // Guess a letter
                char input[10];
                int got_input = get_input_with_timer(input, sizeof(input), 15, 
                                                     "Enter a letter: ");
                
                if (got_input) {
                    if (strlen(input) == 1 && isalpha(input[0])) {
                        char move_msg[20];
                        snprintf(move_msg, sizeof(move_msg), "LETTER:%c\n", input[0]);
                        send(sock, move_msg, strlen(move_msg), 0);
                        printf("✓ Sent letter: %c\n", input[0]);
                    } else {
                        printf("\nInvalid input! Must be a single letter. Turn wasted.\n");
                        send(sock, "LETTER:X\n", 9, 0);
                    }
                } else {
                    // Timeout - don't send anything, server will timeout
                }

            } else if (choice == 2) {
                // Guess the word
                char word[WORD_LEN];
                int got_input = get_input_with_timer(word, sizeof(word), 15, 
                                                     "Enter the word: ");
                
                if (got_input) {
                    char move_msg[30];
                    snprintf(move_msg, sizeof(move_msg), "WORD:%s\n", word);
                    send(sock, move_msg, strlen(move_msg), 0);
                    printf("✓ Sent word: %s\n", word);
                } else {
                    // Timeout - don't send anything
                }
            }
            
            state.needs_display = 1;
        }
        else if (strcmp(buffer, "TIMEOUT") == 0) {
            clear_screen();
            printf("\n");
            printf("╔═══════════════════════════════════════╗\n");
            printf("║                                       ║\n");
            printf("║          YOU TIMED OUT!               ║\n");
            printf("║                                       ║\n");
            printf("║     Penalty: -1 point applied         ║\n");
            printf("║                                       ║\n");
            printf("╚═══════════════════════════════════════╝\n");
            sleep(2);
            state.needs_display = 1;
        }
        else if (strcmp(buffer, "WAIT") == 0) {
            continue;
        }
        else if (strncmp(buffer, "BOARD:", 6) == 0) {
            strcpy(state.answer_space, buffer + 6);
            state.needs_display = 1;
        }
        else if (strncmp(buffer, "STATE:", 6) == 0) {
            sscanf(buffer + 6, "R%d|L%d|S%d", &state.round, &state.lives, &state.score);
            state.needs_display = 1;
            
            if (state.lives <= 0 && !is_eliminated) {
                is_eliminated = 1;
            }
        }
        else if (strncmp(buffer, "ROUND_SCORES:", 13) == 0) {
            clear_screen();
            printf("\n");
            printf("╔═════════════════════════════════════════════════════════╗\n");
            printf("║                   ROUND %d SUMMARY                      ║\n", state.round);
            printf("╠═════════════════════════════════════════════════════════╣\n");
            
            char scores_copy[256];
            strcpy(scores_copy, buffer + 13);
            char *token = strtok(scores_copy, "|");
            int player_num = 1;
            
            while (token != NULL && player_num <= 5) {
                printf("║ Player %d: %-45s ║\n", player_num, token);
                token = strtok(NULL, "|");
                player_num++;
            }
            
            printf("╚═════════════════════════════════════════════════════════╝\n");
            printf("\nPress Enter to continue...");
            fflush(stdout);
            getchar();
            state.needs_display = 1;
        }
        else if (strcmp(buffer, "INVALID") == 0) {
            printf("\n*** Invalid move! ***\n");
            sleep(1);
            state.needs_display = 1;
        }
        else if (strcmp(buffer, "CORRECT") == 0) {
            printf("\n *** CORRECT! Good guess! *** \n");
            sleep(1);
            state.needs_display = 1;
        }
        else if (strcmp(buffer, "WRONG") == 0) {
            printf("\n *** WRONG! Try again... *** \n");
            sleep(1);
            state.needs_display = 1;
        }
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
            state.needs_display = 0;
            printf("\nYou can still spectate the game...\n");
        }
        else if (strncmp(buffer, "REVEAL:", 7) == 0) {
            clear_screen();
            printf("\n");
            printf("╔═══════════════════════════════════════╗\n");
            printf("║                                       ║\n");
            printf("║             ROUND %d ENDED            ║\n", state.round);
            printf("║                                       ║\n");
            printf("║    THE ANSWER WAS: %-18s  ║\n", buffer + 7);
            printf("║                                       ║\n");
            printf("╚═══════════════════════════════════════╝\n");
            state.needs_display = 0;
        }
        else if (strcmp(buffer, "END") == 0) {
            printf("\n");
            printf("╔═══════════════════════════════════════╗\n");
            printf("║                                       ║\n");
            printf("║            GAME ENDED!                ║\n");
            printf("║                                       ║\n");
            printf("║         Final Score: %-16d    ║\n", state.score);
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