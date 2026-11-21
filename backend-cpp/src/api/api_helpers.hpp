#pragma once

#include <cstdint>
#include <exception>
#include <string>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace koma::api {

inline void send_json(httplib::Response& res, const nlohmann::json& j, int status = 200) {
    res.status = status;
    res.set_content(j.dump(), "application/json; charset=utf-8");
}

inline void send_error(httplib::Response& res, const std::string& message, int status = 400) {
    send_json(res, {{"error", message}}, status);
}

inline bool parse_json_body(const httplib::Request& req, nlohmann::json& out, httplib::Response& res) {
    try {
        out = nlohmann::json::parse(req.body);
        return true;
    } catch (const std::exception& ex) {
        send_error(res, ex.what(), 400);
        return false;
    }
}

inline bool parse_json_object(const httplib::Request& req, nlohmann::json& out, httplib::Response& res) {
    if (!parse_json_body(req, out, res)) {
        return false;
    }
    if (!out.is_object()) {
        send_error(res, "Expected JSON object", 400);
        return false;
    }
    return true;
}

inline std::string query_str(const httplib::Request& req, const char* key, const std::string& fallback = {}) {
    return req.has_param(key) ? req.get_param_value(key) : fallback;
}

inline int query_int(const httplib::Request& req, const char* key, int fallback = 0) {
    if (!req.has_param(key)) return fallback;
    try {
        return std::stoi(req.get_param_value(key));
    } catch (...) {
        return fallback;
    }
}

inline int64_t query_int64(const httplib::Request& req, const char* key, int64_t fallback = 0) {
    if (!req.has_param(key)) return fallback;
    try {
        return std::stoll(req.get_param_value(key));
    } catch (...) {
        return fallback;
    }
}

inline bool query_bool(const httplib::Request& req, const char* key, bool fallback = false) {
    if (!req.has_param(key)) return fallback;
    const std::string v = req.get_param_value(key);
    return v == "1" || v == "true" || v == "TRUE" || v == "yes";
}

inline bool match_int64(const httplib::Request& req, size_t index, int64_t& out) {
    if (index >= req.matches.size()) return false;
    try {
        out = std::stoll(req.matches[index].str());
        return true;
    } catch (...) {
        return false;
    }
}

inline bool match_int(const httplib::Request& req, size_t index, int& out) {
    if (index >= req.matches.size()) return false;
    try {
        out = std::stoi(req.matches[index].str());
        return true;
    } catch (...) {
        return false;
    }
}

// Wraps an HTTP handler so uncaught exceptions become structured 400/500 JSON errors.
template <typename Fn>
auto guarded(Fn&& fn) {
    return [fn = std::forward<Fn>(fn)](const httplib::Request& req, httplib::Response& res) {
        try {
            fn(req, res);
        } catch (const std::exception& ex) {
            send_error(res, ex.what(), 400);
        } catch (...) {
            send_error(res, "Unexpected internal error", 500);
        }
    };
}

} // namespace koma::api
