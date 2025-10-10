# Custom Capture Hook Diff (vs. pristine `../uvgRTP`)

## Build system wiring
Inject the customization sources and include paths when we build the locally vendored library.

```diff
--- CMakeLists.txt
+++ CMakeLists.txt
@@
-add_subdirectory(uvgRTP)
+add_subdirectory(uvgRTP)
+
+target_sources(uvgrtp PRIVATE
+    ${CMAKE_SOURCE_DIR}/uvgRTP/customizations/packet_capture.cpp
+)
+
+target_include_directories(uvgrtp PUBLIC
+    ${CMAKE_SOURCE_DIR}/uvgRTP
+    ${CMAKE_SOURCE_DIR}/uvgRTP/customizations
+)
```

## Public API surface (`media_stream`)
Surface the capture helpers without modifying downstream call sites by aliasing the callback type and delegating to the media layer. The `send_raw_rtp_packet` helper stays available for replay.

```diff
--- uvgRTP/include/uvgrtp/media_stream.hh
+++ uvgRTP/include/uvgrtp/media_stream.hh
@@
-#include "util.hh"
-
-#include <unordered_map>
-#include <memory>
+#include "util.hh"
+
+#include <customizations/packet_capture_types.hh>
+
+#include <unordered_map>
+#include <memory>
@@
-            rtp_error_t push_frame(std::unique_ptr<uint8_t[]> data, size_t data_len, uint32_t ts, uint64_t ntp_ts, int rtp_flags);
+            rtp_error_t push_frame(std::unique_ptr<uint8_t[]> data, size_t data_len, uint32_t ts, uint64_t ntp_ts, int rtp_flags);
+
+            using packet_capture_callback = uvgrtp::packet_capture_callback;
+
+            void set_packet_capture(packet_capture_callback cb, bool capture_only);
+            void clear_packet_capture();
+            rtp_error_t send_raw_rtp_packet(const uint8_t* data, size_t len);
```

```diff
--- uvgRTP/src/media_stream.cc
+++ uvgRTP/src/media_stream.cc
@@
-#include <cstring>
-#include <errno.h>
+#include <cstring>
+#include <utility>
+#include <errno.h>
@@
-    return ret;
+    return ret;
+}
+
+void uvgrtp::media_stream::set_packet_capture(packet_capture_callback cb, bool capture_only)
+{
+    if (!media_)
+        return;
+
+    media_->set_packet_capture(std::move(cb), capture_only);
+}
+
+void uvgrtp::media_stream::clear_packet_capture()
+{
+    if (!media_)
+        return;
+
+    media_->set_packet_capture({}, false);
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
 }
```

## Frame queue integration
Instead of storing capture state in the queue itself, we defer to the customization helpers before and after the send loop.

```diff
--- uvgRTP/src/frame_queue.hh
+++ uvgRTP/src/frame_queue.hh
@@
-#include "socket.hh"
-
-#include <atomic>
+#include "socket.hh"
+
+#include <customizations/packet_capture_internal.hh>
+
+#include <atomic>
@@
-            frame_queue(std::shared_ptr<uvgrtp::socket> socket, std::shared_ptr<uvgrtp::rtp> rtp, int rce_flags);
-            ~frame_queue();
+            frame_queue(std::shared_ptr<uvgrtp::socket> socket, std::shared_ptr<uvgrtp::rtp> rtp, int rce_flags);
+            ~frame_queue();
+            using packet_capture_cb = uvgrtp::packet_capture_callback;
@@
-            rtp_error_t flush_queue(sockaddr_in& addr, sockaddr_in6& addr6, uint32_t ssrc);
+            rtp_error_t flush_queue(sockaddr_in& addr, sockaddr_in6& addr6, uint32_t ssrc);
+            void set_packet_capture(packet_capture_cb cb, bool capture_only);
+            void clear_packet_capture();
```

```diff
--- uvgRTP/src/frame_queue.cc
+++ uvgRTP/src/frame_queue.cc
@@
-#include "random.hh"
-#include "debug.hh"
-#include <thread>
+#include "random.hh"
+#include "debug.hh"
+#include <thread>
+#include <utility>
@@
-    if (active_->packets.size() > 1)
-        ((uint8_t *)&active_->rtp_headers[active_->rtphdr_ptr - 1])[1] |= (1 << 7);
-    
-    std::chrono::high_resolution_clock::time_point now = std::chrono::high_resolution_clock::now();
+    if (active_->packets.size() > 1)
+        ((uint8_t *)&active_->rtp_headers[active_->rtphdr_ptr - 1])[1] |= (1 << 7);
+
+    auto capture_reference = std::chrono::steady_clock::now();
+    auto capture_state = uvgrtp::customizations::packet_capture::prepare_frame(
+        *this, active_->packets, active_->rtp_headers, active_->rtphdr_ptr, capture_reference);
+    const bool capture_active = capture_state.active;
+    const bool capture_only = capture_state.capture_only;
+    std::chrono::steady_clock::time_point capture_emit_time = capture_reference;
+    bool capture_time_set = false;
+
+    std::chrono::high_resolution_clock::time_point now = std::chrono::high_resolution_clock::now();
@@
-        for (size_t i = 0; i < active_->packets.size(); ++i)
-        {
-            std::chrono::high_resolution_clock::time_point next_packet = now + i * packet_interval;
-
-            // sleep until next packet time
-            std::this_thread::sleep_for(next_packet - std::chrono::high_resolution_clock::now());
-
-            //  send pkt vects
-            if (socket_->sendto(ssrc, addr, addr6, active_->packets[i], 0) != RTP_OK) {
-                UVG_LOG_ERROR("Failed to send packet: %li", errno);
-                (void)deinit_transaction();
-                return RTP_SEND_ERROR;
-            }
-        }
-
-        if (capture_active && !capture_time_set)
-            capture_emit_time = now;
+        for (size_t i = 0; i < active_->packets.size(); ++i)
+        {
+            std::chrono::high_resolution_clock::time_point next_packet = now + i * packet_interval;
+
+            // sleep until next packet time
+            std::this_thread::sleep_for(next_packet - std::chrono::high_resolution_clock::now());
+
+            if (capture_active && !capture_time_set) {
+                capture_emit_time = std::chrono::steady_clock::now();
+                capture_time_set = true;
+            }
+
+            if (!capture_only) {
+                if (socket_->sendto(ssrc, addr, addr6, active_->packets[i], 0) != RTP_OK) {
+                    UVG_LOG_ERROR("Failed to send packet: %li", errno);
+                    (void)deinit_transaction();
+                    return RTP_SEND_ERROR;
+                }
+            }
+        }
+
+        if (capture_active && !capture_time_set) {
+            capture_emit_time = std::chrono::steady_clock::now();
+            capture_time_set = true;
+        }
@@
-    else if (socket_->sendto(ssrc, addr, addr6, active_->packets, 0) != RTP_OK) {
-        UVG_LOG_ERROR("Failed to flush the message queue: %li", errno);
-        (void)deinit_transaction();
-        return RTP_SEND_ERROR;
-    }
-    
-    //UVG_LOG_DEBUG("full message took %zu chunks and %zu messages", active_->chunk_ptr, active_->hdr_ptr);
-    return deinit_transaction();
+    else {
+        if (!capture_only) {
+            if (socket_->sendto(ssrc, addr, addr6, active_->packets, 0) != RTP_OK) {
+                UVG_LOG_ERROR("Failed to flush the message queue: %li", errno);
+                (void)deinit_transaction();
+                return RTP_SEND_ERROR;
+            }
+        }
+
+        if (capture_active) {
+            capture_emit_time = std::chrono::steady_clock::now();
+            capture_time_set = true;
+        }
+    }
+
+    if (capture_active) {
+        uvgrtp::customizations::packet_capture::emit_frame(
+            *this, std::move(capture_state), capture_time_set ? capture_emit_time : capture_reference);
+    }
+
+    //UVG_LOG_DEBUG("full message took %zu chunks and %zu messages", active_->chunk_ptr, active_->hdr_ptr);
+    return deinit_transaction();
+}
+
+void uvgrtp::frame_queue::set_packet_capture(packet_capture_cb cb, bool capture_only)
+{
+    uvgrtp::customizations::packet_capture::set_capture(*this, std::move(cb), capture_only);
+}
+
+void uvgrtp::frame_queue::clear_packet_capture()
+{
+    uvgrtp::customizations::packet_capture::clear_capture(*this);
 }
```

## Format wrapper (`formats::media`)
Expose a lightweight setter that forwards capture configuration to the queue.

```diff
--- uvgRTP/src/formats/media.hh
+++ uvgRTP/src/formats/media.hh
@@
-#include "uvgrtp/util.hh"
-
-#include <map>
-#include <memory>
+#include "uvgrtp/util.hh"
+
+#include <customizations/packet_capture_types.hh>
+
+#include <map>
+#include <memory>
@@
-                void set_pace(ssize_t numerator, ssize_t denominator);
+                void set_pace(ssize_t numerator, ssize_t denominator);
+                void set_packet_capture(uvgrtp::packet_capture_callback cb, bool capture_only);
```

```diff
--- uvgRTP/src/formats/media.cc
+++ uvgRTP/src/formats/media.cc
@@
-#include <map>
-#include <unordered_map>
+#include <map>
+#include <unordered_map>
+#include <utility>
@@
 void uvgrtp::formats::media::set_pace(ssize_t numerator, ssize_t denominator)
 {
     fqueue_->set_pace(numerator, denominator);
 }
+
+void uvgrtp::formats::media::set_packet_capture(uvgrtp::packet_capture_callback cb, bool capture_only)
+{
+    if (!fqueue_)
+        return;
+
+    if (cb)
+        fqueue_->set_packet_capture(std::move(cb), capture_only);
+    else
+        fqueue_->clear_packet_capture();
 }
```

## Customization module (new)
All capture logic now lives under `uvgRTP/customizations`. The implementation wraps the `frame_queue` hooks in a dedicated namespace and uses a simple registry keyed by `frame_queue*`.

```diff
++ uvgRTP/customizations/packet_capture_types.hh
++ uvgRTP/customizations/packet_capture_internal.hh
++ uvgRTP/customizations/packet_capture.cpp
@@
+namespace uvgrtp::customizations::packet_capture {
+
+using namespace std::chrono;
+
+void set_capture(frame_queue& fq, packet_capture_callback cb, bool capture_only);
+void clear_capture(frame_queue& fq);
+CaptureState prepare_frame(frame_queue& fq,
+    const pkt_vec& packets,
+    const frame::rtp_header* headers,
+    size_t header_count,
+    steady_clock::time_point now);
+void emit_frame(frame_queue& fq,
+    CaptureState&& state,
+    steady_clock::time_point scheduled_time);
+
+} // namespace uvgrtp::customizations::packet_capture
```

