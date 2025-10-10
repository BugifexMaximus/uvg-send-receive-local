#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <vector>

namespace uvgrtp {

    struct capture_frame_metadata {
        uint32_t rtp_timestamp = 0;
        uint16_t first_sequence = 0;
        uint16_t last_sequence = 0;
        std::size_t packet_count = 0;
        std::chrono::steady_clock::time_point captured_at{};
    };

    using packet_capture_callback = std::function<void(std::vector<std::vector<uint8_t>>,
        std::chrono::nanoseconds,
        capture_frame_metadata)>;

} // namespace uvgrtp

