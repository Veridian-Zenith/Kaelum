#include "sigil.hpp"
#include "wayland-protocols/xdg-shell-client-protocol.hpp"
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>
#include <wayland-client.h>
=======
#include <iostream>
>>>>>>> f3a208535ac134d49e379d14c6e49e33196c5e79
#include <print>
#include <vector>
#include <algorithm>
#include <map>
#include <fstream>
#include <cstring>

#ifdef KAELUM_WITH_LEVEL_ZERO
#include <ze_api.h>
#include <ze_intel_gpu.h>
#endif

namespace Kaelum {

Sigil::Sigil() = default;

Sigil::~Sigil() {
<<<<<<< HEAD
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);

