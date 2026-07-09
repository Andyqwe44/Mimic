/**
 * input_methods.h — Public input forwarding API.
 *
 * Each method file exposes a single entry point.
 * Commands.cpp parses the JSON args into InputArgs once, then dispatches.
 */
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include <windows.h>

// ── Parsed input arguments (one-time parse from JSON) ──
struct InputArgs {
    uint64_t hwnd = 0;
    std::string type;       // click|dblclick|move|drag|wheel|keydown|keyup|keypress|combo|text
    std::string method;     // sendinput|winapi|postmessage|driver
    std::string button;     // left|right|middle
    double x_norm = 0.5;   // normalized X (0-1)
    double y_norm = 0.5;   // normalized Y (0-1)
    int vk = 0;            // virtual key code
    std::string keyName;   // e.g. "A", "Enter"
    std::string code;      // e.g. "KeyA", "Enter"
    int delta = 0;         // wheel delta
    bool ctrlKey  = false;
    bool shiftKey = false;
    bool altKey   = false;
    bool metaKey  = false;
    std::string text;      // for text type
    std::vector<std::pair<double, double>> dragPath; // for drag type
};

// ── Parse JSON args string → InputArgs struct ──
InputArgs parse_input_args(const std::string& argsJson);

// ── Per-method entry points ──
// Each receives a validated hWnd and pre-parsed InputArgs.
// Returns JSON string: {"ok":true} or {"ok":false,"error":"..."}

std::string input_sendinput(HWND hWnd, const InputArgs& args);
std::string input_winapi(HWND hWnd, const InputArgs& args);
std::string input_postmessage(HWND hWnd, const InputArgs& args);
std::string input_driver(HWND hWnd, const InputArgs& args);
