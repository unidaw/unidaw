#pragma once

#include <cstdint>

#include "apps/shared_memory.h"

namespace daw {

float* audioOutChannelPtr(void* base,
                          const ShmHeader& header,
                          uint32_t blockIndex,
                          uint32_t channel);

float* audioInChannelPtr(void* base,
                         const ShmHeader& header,
                         uint32_t blockIndex,
                         uint32_t channel);

}  // namespace daw
