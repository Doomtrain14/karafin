#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// Lenient field accessors for server JSON: missing keys, nulls and mismatched types yield the
// fallback; numbers sent as strings (and vice versa) are converted.
namespace karafin::jellyfin::detail {

using Json = nlohmann::json;

// dump() that never throws: invalid UTF-8 is replaced with U+FFFD.
inline std::string dumpSafe(const Json& j, int indent = -1) {
    return j.dump(indent, ' ', false, Json::error_handler_t::replace);
}

inline const Json* field(const Json& j, std::string_view key) {
    if (!j.is_object()) return nullptr;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return nullptr;
    return &*it;
}

inline std::string getStr(const Json& j, std::string_view key, std::string fallback = {}) {
    const Json* v = field(j, key);
    if (!v) return fallback;
    if (v->is_string()) return v->get<std::string>();
    if (v->is_number_integer()) return std::to_string(v->get<int64_t>());
    if (v->is_number()) return std::to_string(v->get<double>());
    if (v->is_boolean()) return v->get<bool>() ? "true" : "false";
    return fallback;
}

inline int64_t getI64(const Json& j, std::string_view key, int64_t fallback = 0) {
    const Json* v = field(j, key);
    if (!v) return fallback;
    if (v->is_number_integer()) return v->get<int64_t>();
    if (v->is_number_float()) {
        const double d = v->get<double>();
        return std::isfinite(d) ? static_cast<int64_t>(d) : fallback;
    }
    if (v->is_string()) {
        const std::string s = v->get<std::string>();
        char* end = nullptr;
        const long long n = std::strtoll(s.c_str(), &end, 10);
        return (end && end != s.c_str()) ? static_cast<int64_t>(n) : fallback;
    }
    if (v->is_boolean()) return v->get<bool>() ? 1 : 0;
    return fallback;
}

inline int getInt(const Json& j, std::string_view key, int fallback = 0) {
    return static_cast<int>(getI64(j, key, fallback));
}

inline double getDouble(const Json& j, std::string_view key, double fallback = 0) {
    const Json* v = field(j, key);
    if (!v) return fallback;
    if (v->is_number()) return v->get<double>();
    if (v->is_string()) {
        const std::string s = v->get<std::string>();
        char* end = nullptr;
        const double d = std::strtod(s.c_str(), &end);
        return (end && end != s.c_str()) ? d : fallback;
    }
    return fallback;
}

inline bool getBool(const Json& j, std::string_view key, bool fallback = false) {
    const Json* v = field(j, key);
    if (!v) return fallback;
    if (v->is_boolean()) return v->get<bool>();
    if (v->is_number()) return v->get<double>() != 0;
    if (v->is_string()) {
        const std::string s = v->get<std::string>();
        if (s == "true" || s == "True" || s == "1") return true;
        if (s == "false" || s == "False" || s == "0") return false;
    }
    return fallback;
}

inline std::vector<std::string> getStrList(const Json& j, std::string_view key) {
    std::vector<std::string> out;
    const Json* v = field(j, key);
    if (!v || !v->is_array()) return out;
    out.reserve(v->size());
    for (const auto& e : *v)
        if (e.is_string()) out.push_back(e.get<std::string>());
    return out;
}

inline const Json& getObj(const Json& j, std::string_view key) {
    static const Json kEmpty = Json::object();
    const Json* v = field(j, key);
    return (v && v->is_object()) ? *v : kEmpty;
}

inline const Json& getArr(const Json& j, std::string_view key) {
    static const Json kEmpty = Json::array();
    const Json* v = field(j, key);
    return (v && v->is_array()) ? *v : kEmpty;
}

}  // namespace karafin::jellyfin::detail
