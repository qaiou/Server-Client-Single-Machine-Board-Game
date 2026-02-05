Word Guessing Game (Server-Client, IPC Deployment)
==================================================

Compilation
-----------
To compile the project, use:
    make

This will generate the executable:
    ./server

To clean up compiled files:
    make clean

Running the Game
----------------
Start the server:
    ./server

Players are forked as child processes internally.
No TCP sockets are used; communication is via POSIX shared memory and process-shared mutexes.

Game Rules Summary
------------------
- Minimum 3 players, maximum 5 players.
- Each player starts with 3 lives.
- Players take turns in Round Robin order.
- A round ends when the word is completely guessed or all players lose their lives.
- The game runs for 5 rounds total.
- Scores are recorded in "scores.txt" after each round.
- Logs are written to "game.log".

Modes Supported
---------------
- IPC with Shared Memory
- Round Robin Scheduling
- Logging Mode

Example Gameplay Flow
---------------------
Round 1 begins:
- The server selects a random 5-letter word (e.g., "APPLE").
- The answer board is initialized as:  _ _ _ _ _
- Player 1 guesses 'A' -> Correct! Board updates to: A _ _ _ _
- Player 2 guesses 'E' -> Correct! Board updates to: A _ _ _ E
- Player 3 guesses 'X' -> Incorrect! Player 3 loses 1 life.
- Play continues until the word is complete or all lives are lost.

End of Round:
- If the word is fully guessed, scores are updated and logged.
- If not the 5th round, the board resets and a new word is selected.
- Current player resets to Player 1.

Final Round (Round 5):
- Once the word is complete, the game ends.
- Final scores are written to "scores.txt".
- Game log is saved in "game_log.txt"