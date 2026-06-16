#pragma once

#include <iostream>
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

    static bool valid(int value) {
        return value >= 0 && value <= 100;
    }
};

inline std::string css_class(int pct) {
    if (pct <= 20) return "critical";
    if (pct <= 40) return "warning";
    return "good";
}

inline void print_disconnected(const std::string& label) {
    std::cout
        << "{\"text\":\"\","
        << "\"tooltip\":\"" << label << " disconnected\","
        << "\"class\":\"disconnected\","
        << "\"alt\":\"disconnected\"}"
        << std::endl;
}

inline void print_battery(const std::string& label, const Battery& battery) {
    int text_level = battery.has_lr() ? battery.average_lr() :
        Battery::valid(battery.left) ? battery.left :
        Battery::valid(battery.right) ? battery.right :
        Battery::valid(battery.case_level) ? battery.case_level : -1;

    if (!Battery::valid(text_level)) {
        std::cout
            << "{\"text\":\"\","
            << "\"tooltip\":\"" << label << " connected (waiting for battery)\","
            << "\"class\":\"good\","
            << "\"alt\":\"connected\"}"
            << std::endl;
        return;
    }

    std::cout
        << "{\"text\":\"" << text_level << "%\","
        << "\"tooltip\":\"" << label;

    if (Battery::valid(battery.left)) std::cout << "\\nL: " << battery.left << "%";
    if (Battery::valid(battery.right)) std::cout << "\\nR: " << battery.right << "%";
    if (Battery::valid(battery.case_level)) std::cout << "\\nCase: " << battery.case_level << "%";

    std::cout
        << "\","
        << "\"class\":\"" << css_class(text_level) << "\","
        << "\"alt\":\"connected\"}"
        << std::endl;
}
