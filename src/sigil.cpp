#include "sigil.hpp"
#include <iostream>
#include <print>


namespace Kaelum {

Sigil::Sigil() = default;
Sigil::~Sigil() = default;

std::expected<void, SigilError> Sigil::initialize() {
    std::println("Sigil: Initializing Vulkan and Intel Level Zero...");
    return {};
}


void Sigil::render(const Nexus& nexus) {
    // Render loop logic goes here
}

} // namespace Kaelum
