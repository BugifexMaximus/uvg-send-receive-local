# Diff Implementation Guide

## include/uvgrtp/util.hh — top-level includes

```diff
@@
-#include <stdint.h>
+#include <chrono>
+#include <cstddef>
+#include <stdint.h>
```

## include/uvgrtp/util.hh — `capture_frame_metadata` addition

```diff
@@
-extern thread_local rtp_error_t rtp_errno;
+extern thread_local rtp_error_t rtp_errno;
+
+namespace uvgrtp {
+
+    struct capture_frame_metadata {
+        uint32_t            rtp_timestamp = 0;
+        uint16_t            first_sequence = 0;
+        uint16_t            last_sequence = 0;
+        std::size_t         packet_count = 0;
+        std::chrono::steady_clock::time_point captured_at{};
+    };
+
+}
```

## include/uvgrtp/media_stream.hh — STL includes

```diff
@@
-#include <unordered_map>
-#include <memory>
-#include <string>
-#include <atomic>
-#include <cstdint>
+#include <cstdint>
+#include <atomic>
+#include <chrono>
+#include <functional>
+#include <memory>
+#include <string>
+#include <unordered_map>
+#include <vector>
```

## include/uvgrtp/media_stream.hh — packet capture declarations

```diff
@@
 rtp_error_t push_frame(std::unique_ptr<uint8_t[]> data, size_t data_len, uint32_t ts, uint64_t ntp_ts, int rtp_flags);
 
+using packet_capture_callback = std::function<void(std::vector<std::vector<uint8_t>>, std::chrono::nanoseconds, capture_frame_metadata)>;
+
+/**
+ * \brief Install callback for observing outgoing RTP packets.
+ *
+ * \details When set, every RTP packet flushed from the frame queue is provided to the callback
+ * along with the intended send offset relative to the start of capture. If capture_only is true,
+ * packets are not transmitted to the network.
+ */
+void set_packet_capture(packet_capture_callback cb, bool capture_only);
+
+/**
+ * \brief Remove capture callback and resume normal sending.
+ */
+void clear_packet_capture();
+
+/**
+ * \brief Send a preconstructed RTP packet through the stream.
+ *
+ * \details The packet must contain full RTP headers as they should appear on the wire.
+ * uvgRTP will apply any active socket handlers (e.g. SRTP) before transmission.
+ */
+rtp_error_t send_raw_rtp_packet(const uint8_t* data, size_t len);
+
 // Disabled for now
 //rtp_error_t push_user_packet(uint8_t* data, uint32_t len);
```

## src/media_stream.cc — STL includes

```diff
@@
-#include <cstring>
-#include <errno.h>
+#include <cstring>
+#include <utility>
+#include <errno.h>
```

## src/media_stream.cc — packet capture plumbing

```diff
@@
 rtp_error_t uvgrtp::media_stream::push_frame(std::unique_ptr<uint8_t[]> data, size_t data_len, uint32_t ts, uint64_t ntp_ts, int rtp_flags)
 {
@@
     return ret;
 }
 
+void uvgrtp::media_stream::set_packet_capture(packet_capture_callback cb, bool capture_only)
+{
+    if (!media_) {
+        return;
+    }
+
+    media_->set_packet_capture(std::move(cb), capture_only);
+}
+
+void uvgrtp::media_stream::clear_packet_capture()
+{
+    if (media_) {
+        media_->set_packet_capture({}, false);
+    }
+}
+
+rtp_error_t uvgrtp::media_stream::send_raw_rtp_packet(const uint8_t* data, size_t len)
+{
+    if (!initialized_) {
+        UVG_LOG_ERROR("RTP context has not been initialized fully, cannot continue!");
+        return RTP_NOT_INITIALIZED;
+    }
+
+    if (!data || !len) {
+        return RTP_INVALID_VALUE;
+    }
+
+    uvgrtp::buf_vec buffers;
+    buffers.emplace_back(len, const_cast<uint8_t*>(data));
+
+    return socket_->sendto(ssrc_.get()->load(), remote_sockaddr_, remote_sockaddr_ip6_, buffers, 0);
+}
+
 /* Disabled for now
 rtp_error_t uvgrtp::media_stream::push_user_packet(uint8_t* data, uint32_t len)
```

## src/formats/media.hh — include stack

```diff
@@
 #include "uvgrtp/util.hh"
 
+#include "../frame_queue.hh"
+
 #include <map>
 #include <memory>
```

## src/formats/media.hh — surface capture setter

```diff
@@
                 void set_fps(ssize_t numerator, ssize_t denominator);
                 void set_pace(ssize_t numerator, ssize_t denominator);
+                void set_packet_capture(frame_queue::packet_capture_cb cb, bool capture_only);
```

## src/formats/media.cc — STL includes

```diff
@@
 #include "debug.hh"
 
 #include <map>
 #include <unordered_map>
+#include <utility>
```

## src/formats/media.cc — forward packet capture

```diff
@@
 void uvgrtp::formats::media::set_pace(ssize_t numerator, ssize_t denominator)
 {
     fqueue_->set_pace(numerator, denominator);
 }
+
+void uvgrtp::formats::media::set_packet_capture(frame_queue::packet_capture_cb cb, bool capture_only)
+{
+    if (fqueue_) {
+        fqueue_->set_packet_capture(std::move(cb), capture_only);
+    }
+}
```

## src/frame_queue.hh — header includes

```diff
@@
-#include <atomic>
-#include <memory>
-#include <unordered_map>
-#include <vector>
-#include <mutex>
-#include <chrono>
+#include <atomic>
+#include <chrono>
+#include <functional>
+#include <memory>
+#include <mutex>
+#include <unordered_map>
+#include <vector>
```

## src/frame_queue.hh — capture callback type and API

```diff
@@
             frame_queue(std::shared_ptr<uvgrtp::socket> socket, std::shared_ptr<uvgrtp::rtp> rtp, int rce_flags);
             ~frame_queue();
 
+            using packet_capture_cb = std::function<void(std::vector<std::vector<uint8_t>>, std::chrono::nanoseconds, capture_frame_metadata)>;
+
             rtp_error_t init_transaction(bool use_old_rtp_ts);
@@
             /* Flush the message queue
              *
              * Return RTP_OK on success
              * Return RTP_INVALID_VALUE if "sender" is nullptr or message buffer is empty
              * return RTP_SEND_ERROR if send fails */
             rtp_error_t flush_queue(sockaddr_in& addr, sockaddr_in6& addr6, uint32_t ssrc);
 
+            /* Install or reset packet capture callback. When set, each flushed RTP packet is
+             * provided to the callback along with its intended send offset. If capture_only is true,
+             * packets are not transmitted to the network. */
+            void set_packet_capture(packet_capture_cb cb, bool capture_only);
+            void clear_packet_capture();
+
             /* Media may have extra headers (f.ex. NAL and FU headers for HEVC).
```

## src/frame_queue.hh — private helpers and state

```diff
@@
             inline std::chrono::high_resolution_clock::time_point this_frame_time();
 
             inline void update_sync_point();
 
+            std::vector<uint8_t> flatten_packet(const uvgrtp::buf_vec& packet) const;
+            void emit_capture(std::vector<std::vector<uint8_t>>&& packets,
+                const std::chrono::high_resolution_clock::time_point& scheduled_time,
+                capture_frame_metadata meta);
+
             transaction_t *active_;
@@
             std::chrono::high_resolution_clock::time_point fps_sync_point_;
             uint64_t frames_since_sync_ = 0;
 
             bool force_sync_ = false;
+
+            packet_capture_cb capture_cb_;
+            bool capture_only_ = false;
+            bool capture_has_base_time_ = false;
+            std::chrono::high_resolution_clock::time_point capture_base_time_;
```

## src/frame_queue.cc — header includes

```diff
@@
 #include "debug.hh"
 #include <thread>
+#include <utility>
```

## src/frame_queue.cc — capture metadata initialization

```diff
@@
     ++frames_since_sync_;
     }
 
+    bool capture_active = static_cast<bool>(capture_cb_);
+    capture_frame_metadata capture_meta;
+    if (capture_active) {
+        capture_meta.packet_count = active_->packets.size();
+        capture_meta.captured_at = std::chrono::steady_clock::now();
+        if (capture_meta.packet_count > 0 && active_->rtphdr_ptr > 0) {
+            capture_meta.rtp_timestamp = ntohl(active_->rtp_headers[0].timestamp);
+            capture_meta.first_sequence = ntohs(active_->rtp_headers[0].seq);
+            capture_meta.last_sequence = ntohs(active_->rtp_headers[active_->rtphdr_ptr - 1].seq);
+        }
+    }
+
     if ((rce_flags_ & RCE_PACE_FRAGMENT_SENDING) && fps_ && !force_sync_)
     {
+        std::vector<std::vector<uint8_t>> frame_packets;
+        std::chrono::high_resolution_clock::time_point first_packet_time{};
+        bool capture_time_set = false;
+
+        if (capture_active) {
+            frame_packets.reserve(active_->packets.size());
+        }
+
         // allocate 80% of frame interval for pacing, rest for other processing
         std::chrono::nanoseconds packet_interval = pace_numerator_*frame_interval_/(pace_denominator_*active_->packets.size());
```

## src/frame_queue.cc — paced send loop

```diff
@@
             // sleep until next packet time
             std::this_thread::sleep_for(next_packet - std::chrono::high_resolution_clock::now());
 
-            //  send pkt vects
-            if (socket_->sendto(ssrc, addr, addr6, active_->packets[i], 0) != RTP_OK) {
-                UVG_LOG_ERROR("Failed to send packet: %li", errno);
-                (void)deinit_transaction();
-                return RTP_SEND_ERROR;
-            }
+            if (capture_active) {
+                if (!capture_time_set) {
+                    first_packet_time = next_packet;
+                    capture_time_set = true;
+                }
+                frame_packets.push_back(flatten_packet(active_->packets[i]));
+            }
+
+            if (!capture_only_) {
+                //  send pkt vects
+                if (socket_->sendto(ssrc, addr, addr6, active_->packets[i], 0) != RTP_OK) {
+                    UVG_LOG_ERROR("Failed to send packet: %li", errno);
+                    (void)deinit_transaction();
+                    return RTP_SEND_ERROR;
+                }
+            }
         }
 
+        if (capture_active) {
+            if (!capture_time_set) {
+                first_packet_time = now;
+            }
+            emit_capture(std::move(frame_packets), first_packet_time, capture_meta);
+        }
+
     }
     else {
-        if (socket_->sendto(ssrc, addr, addr6, active_->packets, 0) != RTP_OK) {
-            UVG_LOG_ERROR("Failed to flush the message queue: %li", errno);
-            (void)deinit_transaction();
-            return RTP_SEND_ERROR;
-        }
+        std::vector<std::vector<uint8_t>> frame_packets;
+        if (capture_active) {
+            frame_packets.reserve(active_->packets.size());
+            for (auto& packet : active_->packets) {
+                frame_packets.push_back(flatten_packet(packet));
+            }
+        }
+
+        if (!capture_only_) {
+            if (socket_->sendto(ssrc, addr, addr6, active_->packets, 0) != RTP_OK) {
+                UVG_LOG_ERROR("Failed to flush the message queue: %li", errno);
+                (void)deinit_transaction();
+                return RTP_SEND_ERROR;
+            }
+        }
+
+        if (capture_active) {
+            emit_capture(std::move(frame_packets), now, capture_meta);
+        }
     }
```

## src/frame_queue.cc — helper implementations

```diff
@@
 inline void uvgrtp::frame_queue::update_sync_point()
 {
     //UVG_LOG_DEBUG("Updating framerate sync point");
     frames_since_sync_ = 0;
     fps_sync_point_ = std::chrono::high_resolution_clock::now();
 }
+
+void uvgrtp::frame_queue::set_packet_capture(packet_capture_cb cb, bool capture_only)
+{
+    capture_cb_ = std::move(cb);
+    capture_only_ = capture_only && static_cast<bool>(capture_cb_);
+    capture_has_base_time_ = false;
+}
+
+void uvgrtp::frame_queue::clear_packet_capture()
+{
+    capture_cb_ = nullptr;
+    capture_only_ = false;
+    capture_has_base_time_ = false;
+}
+
+std::vector<uint8_t> uvgrtp::frame_queue::flatten_packet(const uvgrtp::buf_vec& packet) const
+{
+    size_t total = 0;
+    for (const auto& part : packet) {
+        total += part.first;
+    }
+
+    std::vector<uint8_t> data;
+    data.reserve(total);
+    for (const auto& part : packet) {
+        data.insert(data.end(), part.second, part.second + part.first);
+    }
+
+    return data;
+}
+
+void uvgrtp::frame_queue::emit_capture(std::vector<std::vector<uint8_t>>&& packets,
+    const std::chrono::high_resolution_clock::time_point& scheduled_time,
+    capture_frame_metadata meta)
+{
+    if (!capture_cb_ || packets.empty()) {
+        return;
+    }
+
+    if (!capture_has_base_time_) {
+        capture_base_time_ = scheduled_time;
+        capture_has_base_time_ = true;
+    }
+
+    std::chrono::nanoseconds offset{};
+    if (scheduled_time >= capture_base_time_) {
+        offset = std::chrono::duration_cast<std::chrono::nanoseconds>(scheduled_time - capture_base_time_);
+    }
+
+    if (meta.packet_count == 0) {
+        meta.packet_count = packets.size();
+    }
+    if (meta.captured_at.time_since_epoch().count() == 0) {
+        meta.captured_at = std::chrono::steady_clock::now();
+    }
+
+    capture_cb_(std::move(packets), offset, meta);
+}
```

## examples/CMakeLists.txt — trailing newline

```diff
@@
 target_link_libraries(sync_receiver     PRIVATE uvgrtp ${CRYPTOPP_LIB_NAME})
 target_link_libraries(v3c_receiver  PRIVATE uvgrtp ${CRYPTOPP_LIB_NAME})
 target_link_libraries(v3c_sender    PRIVATE uvgrtp ${CRYPTOPP_LIB_NAME})
+
```
