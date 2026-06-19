#include "loom.hpp"
#include <pty.h>
#include <unistd.h>
#include <termios.h>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <sys/eventfd.h>
#include <fcntl.h>

namespace {
    constexpr uint64_t URING_TAG_READ  = 1;
    constexpr uint64_t URING_TAG_WRITE = 2;
}
