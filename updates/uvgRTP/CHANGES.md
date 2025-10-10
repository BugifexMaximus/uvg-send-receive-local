# Changes Introduced for Buffered RTP Capture/Replay

This document records the updates made to the uvgRTP library while adding the
buffering/replay support used by `opencv_send_logger`.

## New packet capture plumbing

- `src/frame_queue.hh`: Added `<chrono>`, `<functional>`, and other headers;
  introduced the `packet_capture_cb` typedef (now emitting one callback per
  frame with `std::vector<std::vector<uint8_t>>` payloads plus
  `capture_frame_metadata`), public `set_packet_capture` / `clear_packet_capture`,
  and private state needed to track capture timing.
- `include/uvgrtp/util.hh`: Added `capture_frame_metadata`, capturing the RTP
  timestamp, first/last sequence numbers, packet count, and creation time of a
  buffered frame.
- `src/frame_queue.cc`: Included `<utility>`; in `flush_queue` gather all RTP
  packets belonging to the same frame before forwarding them to the capture
  callback and optionally skip the actual `sendto` call when `capture_only_` is
  set. Added helpers for flattening a packet (`flatten_packet`) and emitting a
  per-frame capture (`emit_capture`) along with metadata (RTP timestamp, first
  and last sequence numbers, packet count, steady-clock capture time), all in
  host byte order.

## Media front-end propagation

- `src/formats/media.hh` / `src/formats/media.cc`: Included the frame queue
  header directly and exposed `set_packet_capture` so higher layers can enable
  capture.

## Public API surface

- `include/uvgrtp/media_stream.hh`: Pulled in `<vector>`, `<functional>`, and
  related headers; exposed the per-frame `packet_capture_callback`, public
  `set_packet_capture` / `clear_packet_capture` methods, and the new
  `send_raw_rtp_packet` helper for replaying buffered payloads. Metadata about
  each captured frame is surfaced via the new `capture_frame_metadata` struct
  (see `include/uvgrtp/util.hh`).
- `src/media_stream.cc`: Included `<utility>` and wired the new methods to the
  underlying `formats::media` instance and socket `sendto` call.

These are the only changes made to the library sources for this feature.  All
other modifications (examples, build glue, scripts) live outside the `uvgRTP`
tree.
