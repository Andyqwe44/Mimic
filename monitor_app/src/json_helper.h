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
    size_t e = json.find('"', p);
    if (e == std::string::npos) return "";
    return json.substr(p, e - p);
}

inline int json_get_int(const std::string& json, const std::string& key) {
    std::string s = "\"" + key + "\":";
    size_t p = json.find(s);
    if (p == std::string::npos) return 0;
    p += s.length();
    return (int)strtol(json.c_str() + p, nullptr, 10);
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
