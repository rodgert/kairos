// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <nomos/rt/spsc_queue.hpp>

#include <cstddef>
#include <cstdint>

namespace kairos {

// Maximum tap values carried in one snapshot. spectral-peaks exposes 17 outputs;
// 64 leaves headroom for richer analysis/probe modules. A tap providing more than
// this is truncated (Part-1 telemetry, not sample-accurate).
constexpr std::size_t k_max_tap_values = 64;

// A fixed-size, trivially-copyable snapshot of a plugin's tap frame, taken on the
// audio thread right after process(). Carries values by index + the schema epoch;
// the names are joined off the audio thread by the telemetry drain (see main.cpp),
// which reads the tap schema via the rcu-protected graph. Never carries strings —
// copying names on the audio thread would allocate.
struct tap_snapshot {
    std::uint32_t epoch{0}; // schema generation this frame was taken under
    std::uint32_t count{0}; // number of valid values (<= k_max_tap_values)
    float         values[k_max_tap_values]{};
};

// Queue: pushed by the audio thread (audio_engine / process_thread, throttled to
// ~tap-push-rate-hz), drained by the telemetry/sender thread → msg_tap. Latest-
// wins in spirit: if the drain falls behind, the audio thread's push is dropped
// (stale analysis is discardable), never blocking the audio thread.
constexpr std::size_t tap_queue_capacity = 64;
using tap_snapshot_queue                 = nomos::rt::spsc_queue<tap_snapshot, tap_queue_capacity>;

} // namespace kairos
