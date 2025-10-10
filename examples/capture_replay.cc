#include <uvgrtp/lib.hh>

#include <chrono>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

struct Options {
    std::string capture_dir;
    std::string ip = "239.0.2.3";
    uint16_t port = 2304;
    bool verbose = false;

    static void print_help(const char* argv0)
    {
        std::cerr << "Usage: " << argv0 << " [options]\n"
                  << "  --capture-dir DIR     Directory containing capture.csv and packet files\n"
                  << "  --ip IP               Destination IP (default: 239.0.2.3)\n"
                  << "  --port N              Destination port (default: 2304)\n"
                  << "  --verbose | -v        Verbose logging\n"
                  << "  -h | --help           This help\n";
    }

    bool parse(int argc, char** argv)
    {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto need = [&](const char* name) -> bool {
                if (i + 1 >= argc) {
                    std::cerr << name << " requires a value\n";
                    return false;
                }
                return true;
            };

            if (a == "--capture-dir") {
                if (!need("--capture-dir"))
                    return false;
                capture_dir = argv[++i];
            } else if (a == "--ip") {
                if (!need("--ip"))
                    return false;
                ip = argv[++i];
            } else if (a == "--port") {
                if (!need("--port"))
                    return false;
                port = static_cast<uint16_t>(std::stoi(argv[++i]));
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

        if (capture_dir.empty()) {
            std::cerr << "--capture-dir is required\n";
            return false;
        }
        if (port == 0) {
            std::cerr << "port must be > 0\n";
            return false;
        }
        return true;
    }
};

struct ReplayFrame {
    std::chrono::nanoseconds offset{0};
    uint32_t original_rtp_ts = 0;
    uint32_t rtp_ts = 0;
    uint16_t first_seq = 0;
    uint16_t last_seq = 0;
    std::vector<std::string> packet_files;
};

static std::vector<std::string> split(const std::string& value, char delim)
{
    std::vector<std::string> result;
    std::stringstream ss(value);
    std::string item;
    while (std::getline(ss, item, delim)) {
        if (!item.empty())
            result.push_back(item);
    }
    return result;
}

static bool load_capture(const Options& opt, std::vector<ReplayFrame>& out_frames)
{
    fs::path base(opt.capture_dir);
    fs::path csv_path = base / "capture.csv";

    std::ifstream csv(csv_path);
    if (!csv) {
        std::cerr << "Failed to open capture CSV: " << csv_path << "\n";
        return false;
    }

    std::string line;
    if (!std::getline(csv, line)) {
        std::cerr << "Capture CSV is empty\n";
        return false;
    }

    size_t expected_index = 1;
    while (std::getline(csv, line)) {
        if (line.empty())
            continue;

        std::vector<std::string> fields;
        fields.reserve(9);
        std::stringstream ss(line);
        std::string field;
        while (std::getline(ss, field, ',')) {
            fields.push_back(field);
        }

        if (fields.size() < 9) {
            std::cerr << "Malformed CSV line: " << line << "\n";
            return false;
        }

        size_t frame_index = static_cast<size_t>(std::stoul(fields[0]));
        if (frame_index != expected_index) {
            std::cerr << "Unexpected frame index " << frame_index << " in CSV, expected "
                      << expected_index << "\n";
            return false;
        }

        ReplayFrame frame;
        frame.offset = std::chrono::nanoseconds(std::stoll(fields[1]));
        frame.original_rtp_ts = static_cast<uint32_t>(std::stoul(fields[3]));
        frame.rtp_ts = static_cast<uint32_t>(std::stoul(fields[4]));
        frame.first_seq = static_cast<uint16_t>(std::stoul(fields[5]));
        frame.last_seq = static_cast<uint16_t>(std::stoul(fields[6]));
        size_t packet_count = static_cast<size_t>(std::stoul(fields[7]));
        frame.packet_files = split(fields[8], '|');

        if (frame.packet_files.size() != packet_count) {
            std::cerr << "Warning: packet count mismatch for frame " << frame_index
                      << " (csv=" << packet_count << ", files=" << frame.packet_files.size() << ")\n";
        }

        out_frames.push_back(std::move(frame));
        ++expected_index;
    }

    if (out_frames.empty()) {
        std::cerr << "No frames found in capture CSV\n";
        return false;
    }

    return true;
}

static bool read_packet(const fs::path& path, std::vector<uint8_t>& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;

    file.seekg(0, std::ios::end);
    std::streamsize size = file.tellg();
    if (size < 0)
        return false;
    file.seekg(0, std::ios::beg);

    out.resize(static_cast<size_t>(size));
    if (size > 0) {
        file.read(reinterpret_cast<char*>(out.data()), size);
        if (!file)
            return false;
    }

    return true;
}

int main(int argc, char** argv)
{
    Options opt;
    if (!opt.parse(argc, argv))
        return 1;

    std::vector<ReplayFrame> frames;
    if (!load_capture(opt, frames))
        return 2;

    fs::path base(opt.capture_dir);

    uvgrtp::context ctx;
    std::unique_ptr<uvgrtp::session, std::function<void(uvgrtp::session*)>> sess(
        ctx.create_session(opt.ip),
        [&](uvgrtp::session* s) {
            if (s)
                ctx.destroy_session(s);
        });

    if (!sess) {
        std::cerr << "create_session failed\n";
        return 3;
    }

    std::unique_ptr<uvgrtp::media_stream, std::function<void(uvgrtp::media_stream*)>> stream(
        sess->create_stream(opt.port, RTP_FORMAT_H264, RCE_SEND_ONLY),
        [&](uvgrtp::media_stream* m) {
            if (m)
                sess->destroy_stream(m);
        });

    if (!stream) {
        std::cerr << "create_stream failed\n";
        return 4;
    }

    const auto start = std::chrono::steady_clock::now();

    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& frame = frames[i];
        std::this_thread::sleep_until(start + frame.offset);

        for (const auto& file_name : frame.packet_files) {
            fs::path pkt_path = base / file_name;
            std::vector<uint8_t> data;
            if (!read_packet(pkt_path, data)) {
                std::cerr << "Failed to read packet: " << pkt_path << "\n";
                return 5;
            }

            rtp_error_t err = stream->send_raw_rtp_packet(data.data(), data.size());
            if (err != RTP_OK) {
                std::cerr << "send_raw_rtp_packet failed for " << pkt_path << " with code " << err << "\n";
                return 6;
            }
        }

        if (opt.verbose) {
            std::cerr << "Replayed frame " << (i + 1) << " / " << frames.size()
                      << " (rtp_ts=" << frame.rtp_ts;
            if (frame.rtp_ts != frame.original_rtp_ts)
                std::cerr << ", original=" << frame.original_rtp_ts;
            std::cerr << ", seq=" << frame.first_seq << "-" << frame.last_seq << ")\n";
        }
    }

    if (opt.verbose)
        std::cerr << "Replay finished\n";

    return 0;
}
