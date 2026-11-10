// Copyright (c) 2025 ChenKS12138

#pragma once

#include <cstdint>

namespace AirBeamCore {

namespace raop {
constexpr uint64_t kSampleRate44100 = 44'100;
// Used when the receiver does not report Audio-Latency (HomePod 27.x). In frames.
#ifndef AIRBEAM_DEFAULT_LATENCY
#define AIRBEAM_DEFAULT_LATENCY 0
#endif
constexpr uint64_t kDefaultAudioLatency = AIRBEAM_DEFAULT_LATENCY;
constexpr uint64_t kPCMChunkLength = 352;
}  // namespace raop
}  // namespace AirBeamCore
