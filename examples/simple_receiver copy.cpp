#include <uvgrtp/lib.hh>

#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

struct ProgramOptions {
    std::string remote_ip = "239.0.2.3";
    std::string bind_ip;
    uint16_t local_port = 2304;
    uint16_t remote_port = 2304;
    int max_frames = -1;
    int wait_timeout_ms = 100;
    int total_timeout_ms = 0;
    bool verbose = false;

    static void print_help(const char *argv0)
    {
        std::cerr << "Usage: " << argv0 << " [options]\n"
                  << "  --remote IP           Remote sender IP (default: 239.0.2.3)\n"
                  << "  --bind IP             Local interface to bind (default: auto)\n"
                  << "  --port N              Local port to listen (default: 2304)\n"
                  << "  --remote-port N       Sender port if different (default: 2304)\n"
                  << "  --max-frames N        Stop after N frames (default: unlimited)\n"
                  << "  --wait-ms N           Poll timeout in ms (default: 100)\n"
                  << "  --total-timeout-ms N  Exit after N ms without completing (default: unlimited)\n"
                  << "  --verbose | -v        Verbose logging\n"
                  << "  -h | --help           This help\n";
    }

    bool parse(int argc, char **argv)
    {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto need = [&](const char *name) -> bool {
                if (i + 1 >= argc) {
                    std::cerr << name << " requires a value\n";
                    return false;
                }
                return true;
            };
            if (a == "--remote") {
                if (!need("--remote"))
                    return false;
                remote_ip = argv[++i];
            } else if (a == "--bind") {
                if (!need("--bind"))
                    return false;
                bind_ip = argv[++i];
            } else if (a == "--port") {
                if (!need("--port"))
                    return false;
                local_port = static_cast<uint16_t>(std::stoi(argv[++i]));
            } else if (a == "--remote-port") {
                if (!need("--remote-port"))
                    return false;
                remote_port = static_cast<uint16_t>(std::stoi(argv[++i]));
            } else if (a == "--max-frames") {
                if (!need("--max-frames"))
                    return false;
                max_frames = std::stoi(argv[++i]);
            } else if (a == "--wait-ms") {
                if (!need("--wait-ms"))
                    return false;
                wait_timeout_ms = std::stoi(argv[++i]);
            } else if (a == "--total-timeout-ms") {
                if (!need("--total-timeout-ms"))
                    return false;
                total_timeout_ms = std::stoi(argv[++i]);
            } else if (a == "--verbose" || a == "-v") {
                verbose = true;
            } else if (a == "-h" || a == "--help") {
                print_help(argv[0]);
                return false;
            } else {
                std::cerr << "Unknown arg: " << a << "\n";
                print_help(argv[0]);
                return false;
            }
        }
        if (local_port == 0) {
            std::cerr << "port must be > 0\n";
            return false;
        }
        if (wait_timeout_ms < 0) {
            std::cerr << "wait-ms must be >= 0\n";
            return false;
        }
        if (total_timeout_ms < 0) {
            std::cerr << "total-timeout-ms must be >= 0\n";
            return false;
        }
        return true;
    }
};

static void log_frame(const uvgrtp::frame::rtp_frame *frame, int index, bool verbose)
{
    if (!verbose)
        return;
    std::cerr << "recv idx=" << index << " ts90=" << frame->header.timestamp << " size=" << frame->payload_len
              << (frame->header.marker ? " [M]\n" : "\n");
}

int main(int argc, char **argv)
{
    ProgramOptions opt;
    if (!opt.parse(argc, argv))
        return 1;

    uvgrtp::context ctx;

    auto session_deleter = [&](uvgrtp::session *s) {
        if (s)
            ctx.destroy_session(s);
    };
    std::unique_ptr<uvgrtp::session, decltype(session_deleter)> session(nullptr, session_deleter);

    if (opt.bind_ip.empty()) {
        session.reset(ctx.create_session(opt.remote_ip));
    } else {
        std::pair<std::string, std::string> addrs(opt.bind_ip, opt.remote_ip);
        session.reset(ctx.create_session(addrs));
    }
    if (!session) {
        std::cerr << "create_session failed\n";
        return 2;
    }

    auto stream_deleter = [&](uvgrtp::media_stream *m) {
        if (m)
            session->destroy_stream(m);
    };
    std::unique_ptr<uvgrtp::media_stream, decltype(stream_deleter)> stream(nullptr, stream_deleter);

    if (opt.remote_port == 0 || opt.remote_port == opt.local_port) {
        stream.reset(session->create_stream(opt.local_port, RTP_FORMAT_H264, RCE_RECEIVE_ONLY));
    } else {
        stream.reset(session->create_stream(opt.local_port, opt.remote_port, RTP_FORMAT_H264, RCE_RECEIVE_ONLY));
    }

    if (!stream) {
        std::cerr << "create_stream failed\n";
        return 3;
    }

    if (opt.verbose) {
        std::cerr << "Listening on port " << opt.local_port << " for RTP H.264 from " << opt.remote_ip;
        if (!opt.bind_ip.empty())
            std::cerr << " (bind " << opt.bind_ip << ")";
        std::cerr << "\n";
    }

    int received = 0;
    auto last_progress = std::chrono::steady_clock::now();
    while (opt.max_frames < 0 || received < opt.max_frames) {
        uvgrtp::frame::rtp_frame *frame = nullptr;
        if (opt.wait_timeout_ms > 0)
            frame = stream->pull_frame(opt.wait_timeout_ms);
        else
            frame = stream->pull_frame();

        if (!frame) {
            if (opt.total_timeout_ms > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - last_progress).count();
                if (elapsed >= opt.total_timeout_ms) {
                    if (opt.verbose)
                        std::cerr << "Timeout waiting for frames (" << elapsed << " ms)\n";
                    return received > 0 ? 0 : 4;
                }
            }
            continue;
        }

        log_frame(frame, received, opt.verbose);
        uvgrtp::frame::dealloc_frame(frame);
        ++received;
        last_progress = std::chrono::steady_clock::now();
    }

    if (opt.verbose) {
        std::cerr << "Received " << received << " frames\n";
    }
    return 0;
}
