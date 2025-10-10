#pragma once

#include "packet_capture_types.hh"

#include <chrono>
#include <vector>

#include "socket.hh"

namespace uvgrtp {
    class frame_queue;

    namespace frame {
        struct rtp_header;
    }

    namespace customizations::packet_capture {

        struct CaptureState {
            bool active = false;
            bool capture_only = false;
            std::vector<std::vector<uint8_t>> packets;
            capture_frame_metadata meta{};
        };

        void set_capture(frame_queue& fq, packet_capture_callback cb, bool capture_only);
        void clear_capture(frame_queue& fq);

        CaptureState prepare_frame(frame_queue& fq,
            const pkt_vec& packets,
            const frame::rtp_header* headers,
            size_t header_count,
            std::chrono::steady_clock::time_point now);

        void emit_frame(frame_queue& fq,
            CaptureState&& state,
            std::chrono::steady_clock::time_point scheduled_time);

    } // namespace customizations::packet_capture
} // namespace uvgrtp

