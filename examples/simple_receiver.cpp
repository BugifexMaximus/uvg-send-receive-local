#include <uvgrtp/lib.hh>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <system_error>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace fs = std::filesystem;

struct AVCodecContextDel {
    void operator()(AVCodecContext* ctx) const
    {
        if (ctx)
            avcodec_free_context(&ctx);
    }
};

struct AVFrameDel {
    void operator()(AVFrame* frame) const
    {
        if (frame)
            av_frame_free(&frame);
    }
};

struct SwsContextDel {
    void operator()(SwsContext* sws) const
    {
        if (sws)
            sws_freeContext(sws);
    }
};

struct ProgramOptions {
    std::string remote_ip = "239.0.2.3";
    std::string bind_ip;
    uint16_t local_port = 2304;
    uint16_t remote_port = 2304;
    int max_frames = -1;
    int total_timeout_ms = 0;
    std::string output_dir = "decoded_frames";
    bool verbose = false;

    static void print_help(const char* argv0)
    {
        std::cerr << "Usage: " << argv0 << " [options]\n"
                  << "  --remote IP           Remote sender IP (default: 239.0.2.3)\n"
                  << "  --bind IP             Local interface to bind (default: auto)\n"
                  << "  --port N              Local port to listen (default: 2304)\n"
                  << "  --remote-port N       Sender port if different (default: 2304)\n"
                  << "  --max-frames N        Stop after N decoded frames (default: unlimited)\n"
                  << "  --total-timeout-ms N  Abort after N ms without finishing (default: unlimited)\n"
                  << "  --output-dir DIR      Directory to store decoded frames (default: decoded_frames)\n"
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
            } else if (a == "--total-timeout-ms") {
                if (!need("--total-timeout-ms"))
                    return false;
                total_timeout_ms = std::stoi(argv[++i]);
            } else if (a == "--output-dir") {
                if (!need("--output-dir"))
                    return false;
                output_dir = argv[++i];
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
        if (remote_port == 0) {
            std::cerr << "remote-port must be > 0\n";
            return false;
        }
        if (max_frames < 0 && max_frames != -1) {
            std::cerr << "max-frames must be >= 0 or -1\n";
            return false;
        }
        if (total_timeout_ms < 0) {
            std::cerr << "total-timeout-ms must be >= 0\n";
            return false;
        }
        return true;
    }
};

struct Receiver {
    explicit Receiver(const ProgramOptions& opt)
        : opt_(opt)
        , output_dir_(opt.output_dir)
    {
    }

    ~Receiver()
    {
        shutdown();
    }

    bool init()
    {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) {
            std::cerr << "H.264 decoder not available\n";
            return false;
        }
        decoder_.reset(avcodec_alloc_context3(codec));
        if (!decoder_) {
            std::cerr << "avcodec_alloc_context3 failed\n";
            return false;
        }
        decoder_->thread_count = 0;
        decoder_->thread_type = FF_THREAD_SLICE;

        if (avcodec_open2(decoder_.get(), codec, nullptr) < 0) {
            std::cerr << "avcodec_open2 failed\n";
            return false;
        }

        frame_.reset(av_frame_alloc());
        if (!frame_) {
            std::cerr << "av_frame_alloc failed\n";
            return false;
        }
        start_saver();
        return true;
    }

    static void hook(void* arg, uvgrtp::frame::rtp_frame* frame)
    {
        auto* self = static_cast<Receiver*>(arg);
        if (!self || !frame) {
            return;
        }
        if (self->is_finished()) {
            uvgrtp::frame::dealloc_frame(frame);
            return;
        }
        self->on_packet(frame);
        uvgrtp::frame::dealloc_frame(frame);
    }

    void on_packet(uvgrtp::frame::rtp_frame* frame)
    {
        AVPacket pkt;
        av_init_packet(&pkt);
        pkt.data = frame->payload;
        pkt.size = frame->payload_len;
        int err = avcodec_send_packet(decoder_.get(), &pkt);
        if (err < 0) {
            if (opt_.verbose) {
                std::cerr << "avcodec_send_packet error: " << err << "\n";
            }
            return;
        }

        while (true) {
            int r = avcodec_receive_frame(decoder_.get(), frame_.get());
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
                break;
            if (r < 0) {
                finish(r);
                return;
            }
            handle_frame(*frame_.get());
            av_frame_unref(frame_.get());
        }
    }

    void handle_frame(const AVFrame& f)
    {
        AVPixelFormat src_fmt = static_cast<AVPixelFormat>(f.format);
        if (!convert_.sws || convert_.width != f.width || convert_.height != f.height || convert_.fmt != src_fmt) {
            convert_.sws.reset(sws_getContext(
                f.width,
                f.height,
                src_fmt == AV_PIX_FMT_NONE ? AV_PIX_FMT_YUVJ420P : src_fmt,
                f.width,
                f.height,
                AV_PIX_FMT_RGB24,
                SWS_BILINEAR,
                nullptr,
                nullptr,
                nullptr));
            if (!convert_.sws) {
                finish(-2);
                return;
            }
            convert_.width = f.width;
            convert_.height = f.height;
            convert_.fmt = src_fmt;
            rgb_.resize(static_cast<size_t>(f.width) * f.height * 3);
        }

        uint8_t* dst_data[4] = { rgb_.data(), nullptr, nullptr, nullptr };
        int dst_linesize[4] = { f.width * 3, 0, 0, 0 };

        if (sws_scale(convert_.sws.get(), f.data, f.linesize, 0, f.height, dst_data, dst_linesize) <= 0) {
            finish(-3);
            return;
        }

        int frame_index = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            frame_index = decoded_;
            ++decoded_;
            if (opt_.verbose) {
                std::cerr << "decoded frame " << decoded_ << " (" << f.width << "x" << f.height << ")\n";
            }
            if (opt_.max_frames > 0 && decoded_ >= opt_.max_frames) {
                finished_ = true;
                exit_code_ = 0;
            }
        }
        enqueue_save_job(frame_index, f.width, f.height);
        cv_.notify_one();
    }

    int wait_until_done()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (finished_)
            return exit_code_;
        if (opt_.total_timeout_ms > 0) {
            if (!cv_.wait_for(lock, std::chrono::milliseconds(opt_.total_timeout_ms), [&] { return finished_; })) {
                finished_ = true;
                exit_code_ = decoded_ > 0 ? 0 : 1;
            }
        } else {
            cv_.wait(lock, [&] { return finished_; });
        }
        return exit_code_;
    }

    void finish(int code)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (finished_)
                return;
            finished_ = true;
            exit_code_ = code;
        }
        cv_.notify_one();
    }

    bool is_finished()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return finished_;
    }

    void shutdown()
    {
        shutdown_saver_internal();
    }

private:
    struct ConvertState {
        int width = 0;
        int height = 0;
        AVPixelFormat fmt = AV_PIX_FMT_NONE;
        std::unique_ptr<SwsContext, SwsContextDel> sws;
    };

    struct SaveJob {
        int index = 0;
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgb;
    };

    void start_saver()
    {
        if (output_dir_.empty())
            return;
        {
            std::lock_guard<std::mutex> lock(save_mutex_);
            if (saver_started_)
                return;
            save_stop_ = false;
            save_queue_.clear();
            saver_started_ = true;
        }
        saver_ = std::thread(&Receiver::save_worker, this);
    }

    void enqueue_save_job(int index, int width, int height)
    {
        if (output_dir_.empty())
            return;

        SaveJob job;
        job.index = index;
        job.width = width;
        job.height = height;
        job.rgb.assign(rgb_.begin(), rgb_.end());

        {
            std::lock_guard<std::mutex> lock(save_mutex_);
            save_queue_.push_back(std::move(job));
        }
        save_cv_.notify_one();
    }

    void save_worker()
    {
        std::unique_lock<std::mutex> lock(save_mutex_);
        while (true) {
            save_cv_.wait(lock, [&] { return save_stop_ || !save_queue_.empty(); });
            if (save_stop_ && save_queue_.empty())
                break;
            SaveJob job = std::move(save_queue_.front());
            save_queue_.pop_front();
            lock.unlock();
            write_job(job);
            lock.lock();
        }
    }

    void write_job(const SaveJob& job)
    {
        if (output_dir_.empty())
            return;

        cv::Mat rgb(job.height, job.width, CV_8UC3, const_cast<uint8_t*>(job.rgb.data()));
        cv::Mat bgr;
        cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);

        fs::path path = fs::path(output_dir_) / make_filename(job.index);
        try {
            if (!cv::imwrite(path.string(), bgr) && opt_.verbose) {
                std::cerr << "Failed to write " << path << "\n";
            }
        } catch (const cv::Exception& e) {
            if (opt_.verbose) {
                std::cerr << "imwrite exception for " << path << ": " << e.what() << "\n";
            }
        }
    }

    std::string make_filename(int index) const
    {
        std::ostringstream name;
        name << "frame_" << std::setw(5) << std::setfill('0') << index << ".png";
        return name.str();
    }

    void shutdown_saver_internal()
    {
        std::unique_lock<std::mutex> lock(save_mutex_);
        if (!saver_started_)
            return;
        save_stop_ = true;
        lock.unlock();
        save_cv_.notify_all();
        if (saver_.joinable())
            saver_.join();
        lock.lock();
        save_queue_.clear();
        saver_started_ = false;
        save_stop_ = false;
    }

    ProgramOptions opt_;
    std::unique_ptr<AVCodecContext, AVCodecContextDel> decoder_;
    std::unique_ptr<AVFrame, AVFrameDel> frame_;
    ConvertState convert_;
    std::vector<uint8_t> rgb_;
    std::string output_dir_;

    std::mutex mutex_;
    std::condition_variable cv_;
    int decoded_ = 0;
    bool finished_ = false;
    int exit_code_ = 0;

    std::deque<SaveJob> save_queue_;
    std::mutex save_mutex_;
    std::condition_variable save_cv_;
    bool save_stop_ = false;
    bool saver_started_ = false;
    std::thread saver_;
};

int main(int argc, char** argv)
{
    av_log_set_level(AV_LOG_ERROR);

    ProgramOptions opt;
    if (!opt.parse(argc, argv))
        return 1;

    if (!opt.output_dir.empty()) {
        std::error_code ec;
        fs::create_directories(opt.output_dir, ec);
        if (ec) {
            std::cerr << "Failed to create output directory '" << opt.output_dir << "': " << ec.message() << "\n";
            return 1;
        }
    }

    uvgrtp::context ctx;

    auto session_del = [&](uvgrtp::session* s) {
        if (s)
            ctx.destroy_session(s);
    };
    std::unique_ptr<uvgrtp::session, decltype(session_del)> session(nullptr, session_del);

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

    auto stream_del = [&](uvgrtp::media_stream* s) {
        if (s)
            session->destroy_stream(s);
    };
    std::unique_ptr<uvgrtp::media_stream, decltype(stream_del)> stream(nullptr, stream_del);

    if (opt.remote_port == opt.local_port) {
        stream.reset(session->create_stream(opt.local_port, RTP_FORMAT_H264, RCE_RECEIVE_ONLY));
    } else {
        stream.reset(session->create_stream(opt.local_port, opt.remote_port, RTP_FORMAT_H264, RCE_RECEIVE_ONLY));
    }
    if (!stream) {
        std::cerr << "create_stream failed\n";
        return 3;
    }

    Receiver receiver(opt);
    if (!receiver.init())
        return 4;

    if (stream->install_receive_hook(&receiver, &Receiver::hook) != RTP_OK) {
        std::cerr << "install_receive_hook failed\n";
        return 5;
    }

    if (opt.verbose) {
        std::cerr << "Listening on port " << opt.local_port << " for RTP H.264 from " << opt.remote_ip;
        if (!opt.bind_ip.empty())
            std::cerr << " (bind " << opt.bind_ip << ")";
        std::cerr << "\n";
    }

    int rc = receiver.wait_until_done();
    receiver.shutdown();
    return rc;
}
