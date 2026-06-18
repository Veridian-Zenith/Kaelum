#pragma once

#include <expected>
#include <memory>
#include "common.hpp"
#include "nexus.hpp"

namespace Kaelum {

    enum class SigilError {
        VulkanInitFailed,
        WaylandInitFailed,
        LevelZeroInitFailed,
        AllocationFailed
    };

    /**
     * @brief Sigil is the GPU renderer utilizing Vulkan and Intel Level Zero.
     */
    class Sigil {
    public:
        Sigil();
        ~Sigil();

        /**
         * @brief Initializes the GPU pipeline and Wayland surface.
         */
        std::expected<void, SigilError> initialize();

        /**
         * @brief Renders the current Nexus grid to the screen.
         */
        void render(const Nexus& nexus);

    private:
        // Vulkan handles, Wayland surface, Level Zero device...
    };

} // namespace Kaelum
