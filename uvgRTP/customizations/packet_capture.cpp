#include "packet_capture_internal.hh"

#include "frame_queue.hh"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <mutex>
#include <unordered_map>

namespace {
    struct CaptureConfig {
        uvgrtp::packet_capture_callback callback;
        bool capture_only = false;
        bool has_base_time = false;
        std::chrono::steady_clock::time_point base_time{};
    };

    std::mutex g_mutex;
    std::unordered_map<uvgrtp::frame_queue*, CaptureConfig> g_configs;

    using uvgrtp::buf_vec;

    std::vector<uint8_t> flatten_packet(const buf_vec& packet)
    {
        size_t total = 0;
        for (const auto& segment : packet) {
            total += segment.first;
        }

        std::vector<uint8_t> flat;
        flat.reserve(total);
        for (const auto& segment : packet) {
            flat.insert(flat.end(), segment.second, segment.second + segment.first);
        }
        return flat;
    }
}

namespace uvgrtp::customizations::packet_capture {

using namespace std::chrono;

void set_capture(frame_queue& fq, packet_capture_callback cb, bool capture_only)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!cb) {
        g_configs.erase(&fq);
        return;
    }

    g_configs[&fq] = CaptureConfig {
        std::move(cb),
        capture_only,
        false,
        {}
    };
}

void clear_capture(frame_queue& fq)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_configs.erase(&fq);
}

CaptureState prepare_frame(frame_queue& fq,
    const pkt_vec& packets,
    const frame::rtp_header* headers,
    size_t header_count,
    steady_clock::time_point now)
{
    CaptureState state;

    packet_capture_callback callback;
    bool capture_only = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_configs.find(&fq);
        if (it == g_configs.end() || !it->second.callback) {
            return state;
        }
        callback = it->second.callback;
        capture_only = it->second.capture_only;
    }

    state.active = true;
    state.capture_only = capture_only;
    state.meta.packet_count = packets.size();
    state.meta.captured_at = now;

    if (header_count > 0 && headers) {
        state.meta.rtp_timestamp = ntohl(headers[0].timestamp);
        state.meta.first_sequence = ntohs(headers[0].seq);
        state.meta.last_sequence = ntohs(headers[header_count - 1].seq);
    }

    state.packets.reserve(packets.size());
    for (const auto& packet : packets) {
        state.packets.push_back(flatten_packet(packet));
    }

    return state;
}

void emit_frame(frame_queue& fq,
    CaptureState&& state,
    steady_clock::time_point scheduled_time)
{
    if (!state.active) {
        return;
    }

    packet_capture_callback callback;
    std::chrono::steady_clock::time_point base_time;
    bool has_base_time = false;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_configs.find(&fq);
        if (it == g_configs.end() || !it->second.callback) {
            return;
        }

        auto& config = it->second;
        callback = config.callback;
        if (!config.has_base_time) {
            config.base_time = scheduled_time;
            config.has_base_time = true;
        }
        base_time = config.base_time;
        has_base_time = config.has_base_time;
    }

    if (state.meta.packet_count == 0) {
        state.meta.packet_count = state.packets.size();
    }

    if (state.meta.captured_at.time_since_epoch().count() == 0) {
        state.meta.captured_at = scheduled_time;
    }

    nanoseconds offset{};
    if (has_base_time && scheduled_time >= base_time) {
        offset = duration_cast<nanoseconds>(scheduled_time - base_time);
    }

    callback(std::move(state.packets), offset, std::move(state.meta));
}

} // namespace uvgrtp::customizations::packet_capture
