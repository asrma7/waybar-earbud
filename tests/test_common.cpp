#include <cassert>
#include <iostream>
#include <sstream>
#include <string>

#include "../src/common.hpp"

class CoutCapture {
public:
    CoutCapture() : old_(std::cout.rdbuf(stream_.rdbuf())) {}
    ~CoutCapture() { std::cout.rdbuf(old_); }

    [[nodiscard]] std::string str() const { return stream_.str(); }

private:
    std::ostringstream stream_;
    std::streambuf* old_;
};

int main() {
    {
        Battery battery{82, 80, 45};
        assert(battery.has_lr());
        assert(battery.average_lr() == 81);
    }

    {
        Battery battery{82, -1, 45};
        assert(!battery.has_lr());
        assert(battery.average_lr() == -1);
    }

    {
        CoutCapture capture;
        print_battery("Test Buds", Battery{82, 80, 45});
        std::string out = capture.str();
        assert(out.find("\"text\":\"81%\"") != std::string::npos);
        assert(out.find("L: 82%") != std::string::npos);
        assert(out.find("R: 80%") != std::string::npos);
        assert(out.find("Case: 45%") != std::string::npos);
        assert(out.find("\"class\":\"good\"") != std::string::npos);
    }

    {
        CoutCapture capture;
        print_disconnected("Earbuds");
        std::string out = capture.str();
        assert(out == "{\"text\":\"\",\"tooltip\":\"Earbuds disconnected\",\"class\":\"disconnected\",\"alt\":\"disconnected\"}\n");
    }

    return 0;
}
