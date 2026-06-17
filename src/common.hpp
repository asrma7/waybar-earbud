#pragma once

#include <algorithm>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>

struct Battery {
    int left = -1;
    int right = -1;
    int case_level = -1;

    [[nodiscard]] bool has_lr() const {
        return valid(left) && valid(right);
    }

    [[nodiscard]] int average_lr() const {
        return has_lr() ? (left + right) / 2 : -1;
    }

    [[nodiscard]] int best_lr() const {
        return has_lr() ? std::max(left, right) : -1;
    }

    static bool valid(int value) {
        return value >= 0 && value <= 100;
    }
};

inline std::string css_class(int pct) {
    if (pct <= 20) return "critical";
    if (pct <= 40) return "warning";
    return "good";
}

inline int display_battery(const Battery& battery) {
    return battery.has_lr() ? battery.best_lr() :
        Battery::valid(battery.left) ? battery.left :
        Battery::valid(battery.right) ? battery.right :
        Battery::valid(battery.case_level) ? battery.case_level : -1;
}

inline void append_charge_field(std::ostringstream& out, const char* key, int value) {
    out << "\"" << key << "\":";
    if (Battery::valid(value)) {
        out << value;
    } else {
        out << "null";
    }
}

inline std::string json_escape_value(const std::string& value) {
    std::string escaped;
    for (char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        default:
            escaped += ch;
            break;
        }
    }
    return escaped;
}

inline std::string status_json(const std::string& label, const std::string& status, const std::string& klass) {
    std::ostringstream out;
    out
        << "{\"text\":\"\","
        << "\"device\":\"" << json_escape_value(label) << "\","
        << "\"battery\":null,"
        << "\"left\":null,"
        << "\"right\":null,"
        << "\"case\":null,"
        << "\"status\":\"" << status << "\","
        << "\"class\":\"" << klass << "\","
        << "\"alt\":\"" << status << "\"}";
    return out.str();
}

inline std::string disconnected_json(const std::string& label) {
    return status_json(label, "disconnected", "disconnected");
}

inline std::string battery_json(const std::string& label, const Battery& battery) {
    int text_level = display_battery(battery);
    if (!Battery::valid(text_level)) {
        return status_json(label, "connected", "good");
    }

    std::ostringstream out;
    out
        << "{\"text\":\"" << text_level << "%\","
        << "\"device\":\"" << json_escape_value(label) << "\","
        << "\"battery\":" << text_level << ",";
    append_charge_field(out, "left", battery.left);
    out << ",";
    append_charge_field(out, "right", battery.right);
    out << ",";
    append_charge_field(out, "case", battery.case_level);
    out << ","
        << "\"status\":\"connected\","
        << "\"class\":\"" << css_class(text_level) << "\","
        << "\"alt\":\"connected\"}";
    return out.str();
}

inline std::function<void(const std::string&)>& json_sink() {
    static std::function<void(const std::string&)> sink;
    return sink;
}

inline void set_json_sink(std::function<void(const std::string&)> sink) {
    json_sink() = std::move(sink);
}

inline void print_json(const std::string& json) {
    if (json_sink()) {
        json_sink()(json);
    } else {
        std::cout << json << std::endl;
    }
}

inline void print_disconnected(const std::string& label) {
    print_json(disconnected_json(label));
}

inline void print_battery(const std::string& label, const Battery& battery) {
    print_json(battery_json(label, battery));
}
