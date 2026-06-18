#include <iostream>
#include <vector>
#include <print>
#include <thread>
#include <chrono>
#include "loom.hpp"
#include "nexus.hpp"

int main() {
    Kaelum::Loom loom;
    Kaelum::Nexus nexus;
    
    auto init_res = loom.initialize();
    if (!init_res) {
        std::println(stderr, "Failed to initialize Loom.");
        return 1;
    }

    std::println("Kaelum Loom and Nexus initialized. PTY linked to fish shell.");
    std::println("Press Ctrl+C to exit. Processing output into grid...\n");

    std::vector<uint8_t> buffer(4096);
    
    while (true) {
        auto read_res = loom.poll_read(buffer);
        if (read_res) {
            size_t bytes = *read_res;
            if (bytes > 0) {
                // Feed the data into the Nexus emulator
                auto process_res = nexus.process_input({buffer.data(), bytes});
                if (!process_res) {
                    std::println(stderr, "Nexus parsing error.");
                }
            }
        }
        
        // In a real app, this is the Vulkan frame loop.
        // For now, we just sleep to prevent 100% CPU.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return 0;
}
