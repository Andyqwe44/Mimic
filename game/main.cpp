#include <iostream>
#include <string>

using namespace std;

char board[3][3] = { {'.', '.', '.'}, {'.', '.', '.'}, {'.', '.', '.'} };

void printBoard() {
    cout << "\n  0 1 2\n";
    for (int i = 0; i < 3; i++) {
        cout << i << " ";
        for (int j = 0; j < 3; j++) {
            cout << board[i][j] << " ";
        }
        cout << "\n";
    }
    cout << "\n";
}

bool isValid(int row, int col) {
    return row >= 0 && row < 3 && col >= 0 && col < 3;
}

bool isOccupied(int row, int col) {
    return board[row][col] != '.';
}

bool checkWin(char player) {
    for (int i = 0; i < 3; i++) {
        if (board[i][0] == player && board[i][1] == player && board[i][2] == player) return true;
        if (board[0][i] == player && board[1][i] == player && board[2][i] == player) return true;
    }
    if (board[0][0] == player && board[1][1] == player && board[2][2] == player) return true;
    if (board[0][2] == player && board[1][1] == player && board[2][0] == player) return true;
    return false;
}

bool isDraw() {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            if (board[i][j] == '.') return false;
        }
    }
    return true;
}

bool parseInput(const string& line, int& row, int& col) {
    size_t i = 0;
    while (i < line.size() && (line[i] < '0' || line[i] > '9')) i++;
    if (i == line.size()) return false;
    row = 0;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
        row = row * 10 + (line[i] - '0');
        i++;
    }
    while (i < line.size() && (line[i] < '0' || line[i] > '9')) i++;
    if (i == line.size()) return false;
    col = 0;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
        col = col * 10 + (line[i] - '0');
        i++;
    }
    return true;
}

int main() {
    cout << unitbuf;  // disable buffering for subprocess I/O
    char currentPlayer = 'X';
    string line;

    cout << "=== Tic Tac Toe ===\n";
    cout << "Enter row and column (0-2), e.g.: 1 1\n";
    cout << "Or enter 'q' to quit.\n";
    printBoard();

    while (true) {
        cout << "Player " << currentPlayer << " > ";
        if (!getline(cin, line)) {
            cout << "\nGoodbye!\n";
            break;
        }

        if (line == "q" || line == "quit" || line == "exit") {
            cout << "Goodbye!\n";
            break;
        }

        int row, col;
        if (!parseInput(line, row, col)) {
            cout << "Invalid input. Enter two numbers like: 1 1\n";
            continue;
        }

        if (!isValid(row, col)) {
            cout << "Out of range. Use 0, 1, or 2 only.\n";
            continue;
        }

        if (isOccupied(row, col)) {
            cout << "Cell already taken. Pick another.\n";
            continue;
        }

        board[row][col] = currentPlayer;
        printBoard();

        if (checkWin(currentPlayer)) {
            cout << "Player " << currentPlayer << " wins!\n";
            break;
        }

        if (isDraw()) {
            cout << "It's a draw!\n";
            break;
        }

        currentPlayer = (currentPlayer == 'X') ? 'O' : 'X';
    }

    return 0;
}
