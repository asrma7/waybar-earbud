#pragma once

#include <unistd.h>

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { reset(); }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    [[nodiscard]] int get() const { return fd_; }
    [[nodiscard]] explicit operator bool() const { return fd_ >= 0; }

    void reset(int fd = -1) {
        if (fd_ >= 0) close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};
