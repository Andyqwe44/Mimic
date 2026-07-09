/**
 * json_helper.h — Minimal JSON helpers for WebMessage command dispatch.
 *
 * Deliberately minimal: only handles simple {"key":"val"} and {"key":num} patterns.
 * The WebView2 WebMessage format is controlled — no need for full JSON parser.
 */
#pragma once
#include <string>
#include <cstdlib>

inline uint64_t json_get_uint64(const std::string& json, const std::string& key) {
    std::string s = "\"" + key + "\":";
    size_t p = json.find(s);
    if (p == std::string::npos) return 0;
    p += s.length();
    return strtoull(json.c_str() + p, nullptr, 10);
}

inline std::string json_get_str(const std::string& json, const std::string& key) {
    std::string s = "\"" + key + "\":\"";
    size_t p = json.find(s);
    if (p == std::string::npos) return "";
    p += s.length();
    // Find closing quote, skipping escaped quotes (backslash-quote)
    size_t e = p;
    while (e < json.size()) {
        if (json[e] == '"' && (e == p || json[e-1] != '\\')) break;
        e++;
    }
    if (e >= json.size()) return "";
    return json.substr(p, e - p);
}

inline int json_get_int(const std::string& json, const std::string& key) {
    std::string s = "\"" + key + "\":";
    size_t p = json.find(s);
    if (p == std::string::npos) return 0;
    p += s.length();
    // Handle JSON boolean literals (JS sends true/false, not 1/0)
    if (json.compare(p, 4, "true") == 0) return 1;
    if (json.compare(p, 5, "false") == 0) return 0;
    return (int)strtol(json.c_str() + p, nullptr, 10);
}

inline double json_get_double(const std::string& json, const std::string& key) {
    std::string s = "\"" + key + "\":";
    size_t p = json.find(s);
    if (p == std::string::npos) return 0.0;
    p += s.length();
    return strtod(json.c_str() + p, nullptr);
}

inline bool json_get_bool(const std::string& json, const std::string& key) {
    return json_get_int(json, key) != 0;
}

inline std::string json_get_obj(const std::string& json, const std::string& key) {
    std::string s = "\"" + key + "\":{";
    size_t p = json.find(s);
    if (p == std::string::npos) return "{}";
    p += s.length() - 1;
    int depth = 0;
    size_t e = p;
    while (e < json.length()) {
        if (json[e] == '{') depth++;
        else if (json[e] == '}') { depth--; if (depth == 0) break; }
        e++;
    }
    return json.substr(p, e - p + 1);
}
