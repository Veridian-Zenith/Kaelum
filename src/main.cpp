#include <iostream>
#include <vector>
#include <print>
#include <thread>
#include <chrono>
#include "loom.hpp"


int main() {
    Kaelum::Loom loom;
    
    auto init_res = loom.initialize();
    if (!init_res) {
        std::println(stderr, "Failed to initialize Loom.");
        return 1;
    }

    std::println("Kaelum Loom initialized. PTY linked to fish shell.");
    std::println("Press Ctrl+C to exit. (Reading from PTY...)\n");

    std::vector<uint8_t> buffer(4096);
    
    // Basic test loop: Poll PTY and print to screen
    while (true) {
        auto read_res = loom.poll_read(buffer);
        if (read_res) {
            size_t bytes = *read_res;
            if (bytes > 0) {
                std::cout.write(reinterpret_cast<const char*>(buffer.data()), bytes);
                std::cout.flush();
            }
        }
        
        // Small sleep to prevent 100% CPU in this basic test
        // In the real app, this will be replaced by a Vulkan frame loop.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return 0;
}
