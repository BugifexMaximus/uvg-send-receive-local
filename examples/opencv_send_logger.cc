#include <opencv2/opencv.hpp>

#include <uvgrtp/lib.hh>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <cstring>
#include <fstream>
#include <cstdio>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace fs = std::filesystem;

struct ProgramOptions {
    std::string input_dir = ".";
    std::string pattern = "frame_*.png";
    std::string ip = "239.0.2.3";
    uint16_t port = 2304;
    int fps = 30;
    int bitrate = 1'000'000;
    int gop = 30;
    int max_frames = -1;
    bool verbose = false;
    bool buffered = false;
    bool override_pts = false;
    int override_frame_index = -1;
    uint32_t override_rtp_timestamp = 0;
    std::string capture_dir;

    static void print_help(const char* argv0)
    {
        std::cerr << "Usage: " << argv0 << " [options]\n"
                  << "  --input DIR           Input folder (default: .)\n"
                  << "  --pattern GLOB        Pattern like frame_*.png (default: frame_*.png)\n"
                  << "  --ip IP               Destination IP (default: 239.0.2.3)\n"
                  << "  --port N              Destination port (default: 2304)\n"
                  << "  --fps N               FPS (default: 30)\n"
                  << "  --bitrate N           Bitrate in bps (default: 1000000)\n"
                  << "  --gop N               GOP size / keyint (default: 30)\n"
                  << "  --max-frames N        Max frames to process (default: all)\n"
                  << "  --set-pts IDX VALUE   Override RTP timestamp of frame IDX (1-based index)\n"
                  << "  --capture-dir DIR     Save captured frames and metadata to DIR\n"
                  << "  --buffered | -b       Buffer RTP packets before sending\n"
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
            if (a == "--input") {
                if (!need("--input"))
                    return false;
                input_dir = argv[++i];
            } else if (a == "--pattern") {
                if (!need("--pattern"))
                    return false;
                pattern = argv[++i];
            } else if (a == "--ip") {
                if (!need("--ip"))
                    return false;
                ip = argv[++i];
            } else if (a == "--port") {
                if (!need("--port"))
                    return false;
                port = static_cast<uint16_t>(std::stoi(argv[++i]));
            } else if (a == "--fps") {
                if (!need("--fps"))
                    return false;
                fps = std::stoi(argv[++i]);
            } else if (a == "--bitrate") {
                if (!need("--bitrate"))
                    return false;
                bitrate = std::stoi(argv[++i]);
            } else if (a == "--gop") {
                if (!need("--gop"))
                    return false;
                gop = std::stoi(argv[++i]);
            } else if (a == "--max-frames") {
                if (!need("--max-frames"))
                    return false;
                max_frames = std::stoi(argv[++i]);
            } else if (a == "--set-pts") {
                if (i + 2 >= argc) {
                    std::cerr << "--set-pts requires two arguments\n";
                    return false;
                }
                override_pts = true;
                override_frame_index = std::stoi(argv[++i]);
                override_rtp_timestamp = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (a == "--capture-dir") {
                if (!need("--capture-dir"))
                    return false;
                capture_dir = argv[++i];
            } else if (a == "--buffered" || a == "-b") {
                buffered = true;
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
        if (fps <= 0) {
            std::cerr << "fps must be > 0\n";
            return false;
        }
        if (bitrate <= 0) {
            std::cerr << "bitrate must be > 0\n";
            return false;
        }
        if (gop <= 0) {
            std::cerr << "gop must be > 0\n";
            return false;
        }
        if (override_pts && override_frame_index <= 0) {
            std::cerr << "--set-pts index must be >= 1\n";
            return false;
        }
        return true;
    }
};

static std::regex glob_to_regex(const std::string& pat)
{
    std::string rx;
    rx.reserve(pat.size() * 2);
    for (char c : pat) {
        switch (c) {
        case '*':
            rx += ".*";
            break;
        case '?':
            rx += '.';
            break;
        case '.':
            rx += "\\.";
            break;
        default:
            rx += c;
            break;
        }
    }
    return std::regex("^" + rx + "$", std::regex::ECMAScript | std::regex::icase);
}

static bool looks_annexb(const uint8_t* p, int n)
{
    if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)
        return true;
    if (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1)
        return true;
    return false;
}

struct AVCodecContextDel {
    void operator()(AVCodecContext* c) const
    {
        if (c)
            avcodec_free_context(&c);
    }
};
struct AVFrameDel {
    void operator()(AVFrame* f) const
    {
        if (f)
            av_frame_free(&f);
    }
};
struct AVPacketDel {
    void operator()(AVPacket* p) const
    {
        if (p)
            av_packet_free(&p);
    }
};
struct SwsContextDel {
    void operator()(SwsContext* s) const
    {
        if (s)
            sws_freeContext(s);
    }
};
struct AVBSFContextDel {
    void operator()(AVBSFContext* b) const
    {
        if (b)
            av_bsf_free(&b);
    }
};

struct SPSPPS {
    std::vector<uint8_t> sps, pps;
    int nal_len_size = 4;
};

struct CapturedFrame {
    std::chrono::nanoseconds offset{0};
    std::chrono::steady_clock::time_point captured_at{};
    uint32_t original_rtp_timestamp = 0;
    uint32_t rtp_timestamp = 0;
    uint16_t first_sequence = 0;
    uint16_t last_sequence = 0;
    std::size_t packet_count = 0;
    std::vector<std::vector<uint8_t>> packets;
};

static bool overwrite_frame_timestamp(CapturedFrame& frame, uint32_t new_ts)
{
    if (frame.packets.empty())
        return false;

    uint32_t ts_net = htonl(new_ts);
    bool updated = false;
    for (auto& packet : frame.packets) {
        if (packet.size() < 12)
            continue;
        std::memcpy(packet.data() + 4, &ts_net, sizeof(ts_net));
        updated = true;
    }

    if (updated) {
        frame.rtp_timestamp = new_ts;
    }

    return updated;
}

static bool save_capture_to_disk(const std::string& directory, const std::vector<CapturedFrame>& frames)
{
    if (directory.empty())
        return true;

    fs::path base(directory);
    std::error_code ec;
    if (!fs::exists(base, ec)) {
        if (!fs::create_directories(base, ec)) {
            std::cerr << "Failed to create capture directory: " << base << "\n";
            return false;
        }
    } else if (!fs::is_directory(base)) {
        std::cerr << "Capture path is not a directory: " << base << "\n";
        return false;
    }

    const fs::path csv_path = base / "capture.csv";
    std::ofstream csv(csv_path, std::ios::out | std::ios::trunc);
    if (!csv) {
        std::cerr << "Failed to open capture CSV for writing: " << csv_path << "\n";
        return false;
    }

    csv << "frame_index,offset_ns,captured_at_ns,original_rtp_ts,rtp_ts,first_sequence,last_sequence,packet_count,packet_files\n";

    for (size_t idx = 0; idx < frames.size(); ++idx) {
        const CapturedFrame& frame = frames[idx];
        const size_t packet_count = frame.packets.size();

        std::vector<std::string> file_names;
        file_names.reserve(packet_count);

        for (size_t pkt = 0; pkt < packet_count; ++pkt) {
            char name[64];
            std::snprintf(name, sizeof(name), "frame_%04zu_pkt_%03zu.pkt", idx + 1, pkt + 1);
            std::string filename(name);
            file_names.push_back(filename);

            fs::path pkt_path = base / filename;
            std::ofstream pkt_file(pkt_path, std::ios::binary | std::ios::out | std::ios::trunc);
            if (!pkt_file) {
                std::cerr << "Failed to open packet file for writing: " << pkt_path << "\n";
                return false;
            }
            const auto& data = frame.packets[pkt];
            pkt_file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            if (!pkt_file) {
                std::cerr << "Failed to write packet file: " << pkt_path << "\n";
                return false;
            }
        }

        auto offset_ns = frame.offset.count();
        auto captured_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(frame.captured_at.time_since_epoch()).count();

        csv << (idx + 1) << ','
            << offset_ns << ','
            << captured_ns << ','
            << frame.original_rtp_timestamp << ','
            << frame.rtp_timestamp << ','
            << frame.first_sequence << ','
            << frame.last_sequence << ','
            << packet_count << ',';

        for (size_t pkt = 0; pkt < file_names.size(); ++pkt) {
            if (pkt)
                csv << '|';
            csv << file_names[pkt];
        }
        csv << '\n';
    }

    return true;
}

static bool replay_buffered_packets(uvgrtp::media_stream& stream,
    const std::vector<CapturedFrame>& frames,
    bool verbose)
{
    if (frames.empty()) {
        std::cerr << "No buffered packets captured\n";
        return false;
    }

    const auto replay_start = std::chrono::steady_clock::now();

    for (size_t frame_idx = 0; frame_idx < frames.size(); ++frame_idx) {
        const auto& frame = frames[frame_idx];
        std::this_thread::sleep_until(replay_start + frame.offset);

        for (const auto& packet : frame.packets) {
            std::vector<uint8_t> working = packet;
            rtp_error_t er = stream.send_raw_rtp_packet(working.data(), working.size());
            if (er != RTP_OK) {
                std::cerr << "send_raw_rtp_packet failed at frame " << frame_idx << " with code " << er << "\n";
                return false;
            }
        }

        if (verbose) {
            std::cerr << "Replayed frame " << (frame_idx + 1) << " / " << frames.size()
                      << " containing " << frame.packets.size() << " packets"
                      << " (rtp_ts=" << frame.rtp_timestamp;
            if (frame.rtp_timestamp != frame.original_rtp_timestamp) {
                std::cerr << ", original=" << frame.original_rtp_timestamp;
            }
            std::cerr << ", seq=" << frame.first_sequence << "-" << frame.last_sequence << ")\n";
        }
    }

    return true;
}

static SPSPPS parse_avcc_raw(const uint8_t* data, int size)
{
    SPSPPS out;
    if (!data || size < 7 || data[0] != 1)
        return out;
    out.nal_len_size = (data[4] & 0x3) + 1;
    int pos = 5;
    int n = size;
    int num_sps = data[pos++] & 0x1F;
    for (int i = 0; i < num_sps; ++i) {
        if (pos + 2 > n)
            break;
        int len = (data[pos] << 8) | data[pos + 1];
        pos += 2;
        if (pos + len > n)
            break;
        out.sps.assign(data + pos, data + pos + len);
        pos += len;
    }
    if (pos >= n)
        return out;
    int num_pps = data[pos++];
    for (int i = 0; i < num_pps; ++i) {
        if (pos + 2 > n)
            break;
        int len = (data[pos] << 8) | data[pos + 1];
        pos += 2;
        if (pos + len > n)
            break;
        out.pps.assign(data + pos, data + pos + len);
        pos += len;
    }
    return out;
}

static SPSPPS parse_avcc_extradata(const AVCodecContext* c)
{
    if (!c)
        return {};
    return parse_avcc_raw(c->extradata, c->extradata_size);
}

static void avcc_payload_to_annexb(const uint8_t* in, int in_size, int nal_len_size, std::vector<uint8_t>& out)
{
    out.clear();
    int i = 0;
    auto push_sc = [&] {
        out.insert(out.end(), {0, 0, 0, 1});
    };
    while (i + nal_len_size <= in_size) {
        uint32_t nal_len = 0;
        for (int k = 0; k < nal_len_size; ++k)
            nal_len = (nal_len << 8) | in[i + k];
        i += nal_len_size;
        if (nal_len == 0 || i + static_cast<int>(nal_len) > in_size)
            break;
        push_sc();
        out.insert(out.end(), in + i, in + i + nal_len);
        i += nal_len;
    }
}

static void prepend_sps_pps(std::vector<uint8_t>& au, const SPSPPS& hp)
{
    if (hp.sps.empty() || hp.pps.empty())
        return;
    std::vector<uint8_t> out;
    const uint8_t sc[4] = {0, 0, 0, 1};
    out.insert(out.end(), sc, sc + 4);
    out.insert(out.end(), hp.sps.begin(), hp.sps.end());
    out.insert(out.end(), sc, sc + 4);
    out.insert(out.end(), hp.pps.begin(), hp.pps.end());
    out.insert(out.end(), au.begin(), au.end());
    au.swap(out);
}

struct EncodedAU {
    std::vector<uint8_t> bytes;
    int64_t pts = 0;
    bool key = false;
};

class Encoder {
public:
    Encoder() = default;
    ~Encoder() = default;

    bool init(int w, int h, int fps, int bitrate, int gop, bool verbose)
    {
        verbose_ = verbose;

        const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
        if (!codec)
            codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) {
            std::cerr << "No H.264 encoder found\n";
            return false;
        }

        ctx_.reset(avcodec_alloc_context3(codec));
        if (!ctx_) {
            std::cerr << "avcodec_alloc_context3 failed\n";
            return false;
        }

        ctx_->width = w;
        ctx_->height = h;
        ctx_->time_base = AVRational { 1, fps };
        ctx_->framerate = AVRational { fps, 1 };
        ctx_->pix_fmt = AV_PIX_FMT_YUVJ420P;
        ctx_->color_range = AVCOL_RANGE_JPEG;
        ctx_->gop_size = gop;
        ctx_->max_b_frames = 0;
        ctx_->bit_rate = bitrate;
        ctx_->flags &= ~AV_CODEC_FLAG_GLOBAL_HEADER;

        av_opt_set(ctx_->priv_data, "preset", "veryfast", 0);
        av_opt_set(ctx_->priv_data, "tune", "zerolatency", 0);
        av_opt_set(ctx_->priv_data, "repeat-headers", "1", 0);

        if (avcodec_open2(ctx_.get(), codec, nullptr) < 0) {
            std::cerr << "avcodec_open2 failed\n";
            return false;
        }

        frame_.reset(av_frame_alloc());
        frame_->format = ctx_->pix_fmt;
        frame_->width = w;
        frame_->height = h;
        if (av_frame_get_buffer(frame_.get(), 32) < 0) {
            std::cerr << "av_frame_get_buffer failed\n";
            return false;
        }

        sws_.reset(sws_getContext(w, h, AV_PIX_FMT_BGR24, w, h, AV_PIX_FMT_YUVJ420P, SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!sws_) {
            std::cerr << "sws_getContext failed\n";
            return false;
        }

        hp_ = parse_avcc_extradata(ctx_.get());

        const AVBitStreamFilter* f = av_bsf_get_by_name("h264_mp4toannexb");
        if (f) {
            AVBSFContext* raw = nullptr;
            if (av_bsf_alloc(f, &raw) < 0) {
                std::cerr << "av_bsf_alloc failed\n";
                return false;
            }
            bsf_.reset(raw);
            if (avcodec_parameters_from_context(bsf_->par_in, ctx_.get()) < 0) {
                std::cerr << "avcodec_parameters_from_context failed\n";
                return false;
            }
            if (av_bsf_init(bsf_.get()) < 0) {
                std::cerr << "av_bsf_init failed\n";
                return false;
            }
            use_bsf_ = true;
            if ((hp_.sps.empty() || hp_.pps.empty()) && bsf_->par_in) {
                SPSPPS from_par = parse_avcc_raw(bsf_->par_in->extradata, bsf_->par_in->extradata_size);
                if (!from_par.sps.empty())
                    hp_ = from_par;
            }
            if ((hp_.sps.empty() || hp_.pps.empty()) && bsf_->par_out) {
                SPSPPS from_par = parse_avcc_raw(bsf_->par_out->extradata, bsf_->par_out->extradata_size);
                if (!from_par.sps.empty())
                    hp_ = from_par;
            }
        } else {
            use_bsf_ = false;
            if (verbose_) {
                std::cerr << "[warn] BSF not found; using AVCC->AnnexB fallback\n";
            }
        }

        return true;
    }

    bool encode(const cv::Mat& bgr, std::vector<EncodedAU>& out_collector, int64_t pts)
    {
        if (bgr.cols != ctx_->width || bgr.rows != ctx_->height || bgr.channels() != 3) {
            std::cerr << "encode: unexpected frame shape\n";
            return false;
        }

        if (av_frame_make_writable(frame_.get()) < 0) {
            std::cerr << "frame not writable\n";
            return false;
        }

        const uint8_t* src[1] = { bgr.data };
        int src_stride[1] = { static_cast<int>(bgr.step[0]) };
        sws_scale(sws_.get(), src, src_stride, 0, ctx_->height, frame_->data, frame_->linesize);

        frame_->pts = pts;

        if (int e = avcodec_send_frame(ctx_.get(), frame_.get()); e < 0) {
            std::cerr << "avcodec_send_frame: " << e << "\n";
            return false;
        }

        while (true) {
            std::unique_ptr<AVPacket, AVPacketDel> pkt(av_packet_alloc());
            int r = avcodec_receive_packet(ctx_.get(), pkt.get());
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
                break;
            if (r < 0) {
                std::cerr << "avcodec_receive_packet: " << r << "\n";
                return false;
            }

            if (use_bsf_) {
                if (av_bsf_send_packet(bsf_.get(), pkt.get()) < 0) {
                    std::cerr << "bsf_send failed\n";
                    return false;
                }
                av_packet_unref(pkt.get());

                while (true) {
                    std::unique_ptr<AVPacket, AVPacketDel> out(av_packet_alloc());
                    int br = av_bsf_receive_packet(bsf_.get(), out.get());
                    if (br == AVERROR(EAGAIN) || br == AVERROR_EOF)
                        break;
                    if (br < 0) {
                        std::cerr << "bsf_receive failed\n";
                        return false;
                    }

                    append_annexb_payload(out->data, out->size, out->pts, (out->flags & AV_PKT_FLAG_KEY) != 0, out_collector);
                }
            } else {
                std::vector<uint8_t> converted;
                if (looks_annexb(pkt->data, pkt->size)) {
                    append_annexb_payload(pkt->data, pkt->size, pkt->pts, (pkt->flags & AV_PKT_FLAG_KEY) != 0, out_collector);
                } else {
                    avcc_payload_to_annexb(pkt->data, pkt->size, hp_.nal_len_size, converted);
                    append_annexb_payload(converted.data(), static_cast<int>(converted.size()), pkt->pts, (pkt->flags & AV_PKT_FLAG_KEY) != 0, out_collector);
                }
            }
        }
        return true;
    }

    bool flush(std::vector<EncodedAU>& out_collector)
    {
        if (avcodec_send_frame(ctx_.get(), nullptr) < 0)
            return false;
        while (true) {
            std::unique_ptr<AVPacket, AVPacketDel> pkt(av_packet_alloc());
            int r = avcodec_receive_packet(ctx_.get(), pkt.get());
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
                break;
            if (r < 0) {
                std::cerr << "flush receive_packet: " << r << "\n";
                return false;
            }

            if (use_bsf_) {
                if (av_bsf_send_packet(bsf_.get(), pkt.get()) < 0)
                    return false;
                av_packet_unref(pkt.get());
                while (true) {
                    std::unique_ptr<AVPacket, AVPacketDel> out(av_packet_alloc());
                    int br = av_bsf_receive_packet(bsf_.get(), out.get());
                    if (br == AVERROR(EAGAIN) || br == AVERROR_EOF)
                        break;
                    if (br < 0)
                        return false;

                    append_annexb_payload(out->data, out->size, out->pts, (out->flags & AV_PKT_FLAG_KEY) != 0, out_collector);
                }
            } else {
                std::vector<uint8_t> converted;
                if (looks_annexb(pkt->data, pkt->size)) {
                    append_annexb_payload(pkt->data, pkt->size, pkt->pts, (pkt->flags & AV_PKT_FLAG_KEY) != 0, out_collector);
                } else {
                    avcc_payload_to_annexb(pkt->data, pkt->size, hp_.nal_len_size, converted);
                    append_annexb_payload(converted.data(), static_cast<int>(converted.size()), pkt->pts, (pkt->flags & AV_PKT_FLAG_KEY) != 0, out_collector);
                }
            }
        }
        return true;
    }

    AVRational time_base() const { return ctx_->time_base; }
    int width() const { return ctx_->width; }
    int height() const { return ctx_->height; }
    void flush_pending(std::vector<EncodedAU>& out) { emit_pending(out); }

private:
    void append_annexb_payload(const uint8_t* data, int size, int64_t pts, bool key, std::vector<EncodedAU>& out)
    {
        if (!data || size <= 0)
            return;

        if (!has_pending_ || pts != pending_.pts) {
            emit_pending(out);
            pending_.pts = pts;
            pending_.key = key;
            pending_.bytes.clear();
            has_pending_ = true;
        } else if (key) {
            pending_.key = true;
        }
        pending_.bytes.insert(pending_.bytes.end(), data, data + size);
    }

    void emit_pending(std::vector<EncodedAU>& out)
    {
        if (!has_pending_)
            return;
        out.emplace_back(std::move(pending_));
        pending_ = {};
        has_pending_ = false;
    }

    bool verbose_ = false;
    std::unique_ptr<AVCodecContext, AVCodecContextDel> ctx_;
    std::unique_ptr<AVFrame, AVFrameDel> frame_;
    std::unique_ptr<SwsContext, SwsContextDel> sws_;
    std::unique_ptr<AVBSFContext, AVBSFContextDel> bsf_;
    bool use_bsf_ = false;
    SPSPPS hp_;
    EncodedAU pending_;
    bool has_pending_ = false;
};

static bool load_and_preprocess(const ProgramOptions& opt, std::vector<cv::Mat>& frames_out, int& W, int& H)
{
    frames_out.clear();

    if (!fs::exists(opt.input_dir) || !fs::is_directory(opt.input_dir)) {
        std::cerr << "Input dir doesn't exist or not a directory: " << opt.input_dir << "\n";
        return false;
    }

    std::regex rx = glob_to_regex(opt.pattern);
    std::vector<fs::path> list;
    for (auto& p : fs::directory_iterator(opt.input_dir)) {
        if (!p.is_regular_file())
            continue;
        auto name = p.path().filename().string();
        if (std::regex_match(name, rx))
            list.push_back(p.path());
    }
    if (list.empty()) {
        std::cerr << "No files matching pattern in " << opt.input_dir << "\n";
        return false;
    }

    std::sort(list.begin(), list.end());

    int count = 0;
    cv::Mat first;
    for (auto& path : list) {
        if (opt.max_frames >= 0 && count >= opt.max_frames)
            break;

        cv::Mat img = cv::imread(path.string(), cv::IMREAD_COLOR);
        if (img.empty()) {
            std::cerr << "Failed to read: " << path << "\n";
            return false;
        }
        if (first.empty()) {
            first = img;
            W = first.cols;
            H = first.rows;
        }

        if (img.channels() == 1)
            cv::cvtColor(img, img, cv::COLOR_GRAY2BGR);
        else if (img.channels() == 4)
            cv::cvtColor(img, img, cv::COLOR_BGRA2BGR);

        if (img.cols != W || img.rows != H) {
            cv::Mat resized;
            cv::resize(img, resized, cv::Size(W, H), 0, 0, cv::INTER_AREA);
            img = resized;
        }
        frames_out.push_back(std::move(img));
        ++count;
    }

    if (opt.verbose) {
        std::cerr << "Loaded " << frames_out.size() << " frames (" << W << "x" << H << ")\n";
    }
    return !frames_out.empty();
}

static bool send_rtsp_annexb_sequence(const ProgramOptions& opt, const std::vector<EncodedAU>& aus, AVRational enc_tb)
{
    if (aus.empty()) {
        std::cerr << "No encoded frames to send\n";
        return false;
    }

    uvgrtp::context ctx;
    std::unique_ptr<uvgrtp::session, std::function<void(uvgrtp::session*)>> sess(ctx.create_session(opt.ip),
        [&](uvgrtp::session* s) {
            if (s)
                ctx.destroy_session(s);
        });
    if (!sess) {
        std::cerr << "create_session failed\n";
        return false;
    }

    std::unique_ptr<uvgrtp::media_stream, std::function<void(uvgrtp::media_stream*)>> strm(
        sess->create_stream(opt.port, RTP_FORMAT_H264, RCE_SEND_ONLY),
        [&](uvgrtp::media_stream* m) {
            if (m)
                sess->destroy_stream(m);
        });
    if (!strm) {
        std::cerr << "create_stream failed\n";
        return false;
    }

    constexpr int DEFAULT_CLOCK = 90000;
    strm->configure_ctx(RCC_CLOCK_RATE, DEFAULT_CLOCK);
    strm->configure_ctx(RCC_MULTICAST_TTL, 1);
    strm->configure_ctx(RCC_DYN_PAYLOAD_TYPE, 96);
    const int rtp_flags = RTP_H26X_DO_NOT_AGGR | RTP_NO_H26X_SCL;

    std::vector<CapturedFrame> buffered_frames;
    if (opt.buffered) {
        buffered_frames.reserve(aus.size());
        strm->set_packet_capture(
            [&buffered_frames](std::vector<std::vector<uint8_t>> packets, std::chrono::nanoseconds offset, uvgrtp::capture_frame_metadata meta) {
                CapturedFrame frame;
                frame.offset = offset;
                frame.captured_at = meta.captured_at;
                frame.original_rtp_timestamp = meta.rtp_timestamp;
                frame.rtp_timestamp = meta.rtp_timestamp;
                frame.first_sequence = meta.first_sequence;
                frame.last_sequence = meta.last_sequence;
                frame.packet_count = meta.packet_count ? meta.packet_count : packets.size();
                frame.packets = std::move(packets);
                buffered_frames.push_back(std::move(frame));
            },
            true);
        if (opt.verbose) {
            std::cerr << "Buffered mode enabled: initial run will record RTP packets only\n";
        }
    }

    const auto frame_period = std::chrono::duration<double>(1.0 / double(opt.fps));
    auto t0 = std::chrono::steady_clock::now();
    auto next_deadline = t0;

    int sent = 0;
    for (const auto& au : aus) {
        int64_t ts90 = av_rescale_q(au.pts, enc_tb, AVRational { 1, DEFAULT_CLOCK });

        next_deadline += std::chrono::duration_cast<std::chrono::steady_clock::duration>(frame_period);
        std::this_thread::sleep_until(next_deadline);

        rtp_error_t er = strm->push_frame(const_cast<uint8_t*>(au.bytes.data()), au.bytes.size(), static_cast<uint32_t>(ts90), rtp_flags);
        if (er != RTP_OK) {
            std::cerr << "push_frame error " << er << " at index " << sent << "\n";
            return false;
        }

        if (opt.verbose) {
            std::cerr << "sent idx=" << sent << " pts=" << au.pts << " ts90=" << ts90 << " size=" << au.bytes.size()
                      << (au.key ? " [IDR]\n" : "\n");
        }
        ++sent;
    }

    auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cerr << "Done. ";
    if (opt.buffered) {
        std::cerr << "Buffered " << sent << " frames";
    } else {
        std::cerr << "Sent " << sent << " frames";
    }
    std::cerr << " in " << elapsed << " s (target ~" << aus.size() / double(opt.fps) << " s)\n";

    if (opt.buffered) {
        strm->clear_packet_capture();
        size_t total_bytes = 0;
        size_t total_packets = 0;
        for (const auto& frame : buffered_frames) {
            total_packets += frame.packet_count;
            for (const auto& pkt : frame.packets) {
                total_bytes += pkt.size();
            }
        }
        if (opt.verbose) {
            std::cerr << "Captured " << buffered_frames.size() << " frames spanning "
                      << total_packets << " RTP packets totaling " << total_bytes << " bytes\n";
        }

        if (opt.override_pts) {
            int idx = opt.override_frame_index - 1;
            if (idx < 0 || idx >= static_cast<int>(buffered_frames.size())) {
                std::cerr << "--set-pts frame index " << opt.override_frame_index
                          << " out of range (1-" << buffered_frames.size() << ")\n";
                return false;
            }
            if (!overwrite_frame_timestamp(buffered_frames[idx], opt.override_rtp_timestamp)) {
                std::cerr << "Failed to update RTP timestamp for frame " << opt.override_frame_index << "\n";
                return false;
            }
            if (opt.verbose) {
                std::cerr << "Updated frame " << opt.override_frame_index << " RTP timestamp from "
                          << buffered_frames[idx].original_rtp_timestamp << " to "
                          << buffered_frames[idx].rtp_timestamp << "\n";
            }
        }

        if (!opt.capture_dir.empty()) {
            if (!save_capture_to_disk(opt.capture_dir, buffered_frames)) {
                std::cerr << "Failed to save capture to " << opt.capture_dir << "\n";
                return false;
            }
            if (opt.verbose) {
                std::cerr << "Saved capture to " << opt.capture_dir << "\n";
            }
        }

        std::cerr << "Replaying buffered packets...\n";
        if (!replay_buffered_packets(*strm, buffered_frames, opt.verbose)) {
            std::cerr << "Replay failed\n";
            return false;
        }
        std::cerr << "Replay finished successfully\n";
    }

    return true;
}

int main(int argc, char** argv)
{
    av_log_set_level(AV_LOG_ERROR);

    ProgramOptions opt;
    if (!opt.parse(argc, argv))
        return 1;

    if (!opt.buffered && !opt.capture_dir.empty()) {
        std::cerr << "--capture-dir requires --buffered\n";
        return 1;
    }

    std::vector<cv::Mat> frames;
    int W = 0;
    int H = 0;
    if (!load_and_preprocess(opt, frames, W, H))
        return 2;

    Encoder enc;
    if (!enc.init(W, H, opt.fps, opt.bitrate, opt.gop, opt.verbose))
        return 3;

    std::vector<EncodedAU> aus;
    aus.reserve(frames.size() + 8);

    int64_t pts = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < frames.size(); ++i) {
        if (!enc.encode(frames[i], aus, pts++)) {
            std::cerr << "encode failed at " << i << "\n";
            return 4;
        }
    }
    if (!enc.flush(aus)) {
        std::cerr << "flush failed\n";
        return 5;
    }
    enc.flush_pending(aus);
    auto enc_elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    if (opt.verbose) {
        int idr = 0;
        for (auto& au : aus)
            if (au.key)
                ++idr;
        std::cerr << "Encoded " << aus.size() << " AUs (" << idr << " IDR) in " << enc_elapsed << " s\n";
    }

    if (!send_rtsp_annexb_sequence(opt, aus, enc.time_base()))
        return 6;

    return 0;
}
