/**
 * input_driver.cpp — Driver-level input (placeholder).
 *
 * Future: Interception / virtual HID kernel driver.
 */
#include "input_methods.h"
#include <string>

std::string input_driver(HWND /*hWnd*/, const InputArgs& /*args*/) {
    return "{\"ok\":false,\"error\":\"driver-level input not implemented (requires kernel driver)\"}";
}
