/**
 * TicTacToe - Network & TCP client
 *
 * Protocol (newline-delimited, compatible with ai_server.py):
 *   Send: "b0 b1 ... b8 player\n"
 *   Recv: "row col value\n"
 *   End:  "END winner\n"
 */
#pragma once

// Cross-platform socket type
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  using socklen_t = int;
#else
  #include <sys/socket.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR   (-1)
  #define closesocket(s) close(s)
#endif

/** RAII Winsock init/cleanup (Windows only; no-op on other platforms) */
class WinsockGuard {
public:
    WinsockGuard();
    ~WinsockGuard();
    bool ok() const { return ok_; }
private:
    bool ok_ = false;
};

/** Connect to AI server. Returns INVALID_SOCKET on failure. */
SOCKET connect_to_server(const char* host, int port);

/** Send end-of-game message. Returns false on error. */
bool send_end(SOCKET sock, int winner);

/** Get AI move: send board state -> receive row/col/value */
bool get_ai_move(SOCKET sock, char player, int& row, int& col, float& value);
