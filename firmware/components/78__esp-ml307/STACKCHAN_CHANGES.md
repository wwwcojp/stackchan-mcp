# StackChan changes to the vendored esp-ml307

This directory is an unchanged copy of the managed component `78/esp-ml307` 3.6.7
(`repository_info.commit_sha: 40d99e50367034832a6790c17832ca2cb5b91f8b`), taken from the
stage-5 prototype commit ee74637 of the fork without `.component_hash` and `CHECKSUMS.json`.
It is vendored so that FW-A2 can change it (design `docs/superpowers/specs/2026-10-07-fw-a2-design.md`
§4.2 in stackchan-works). `main/idf_component.yml` still asks for `~3.6.5`; the component
manager resolves it to this local copy (`dependencies.lock`: `type: local`).

## Changes

FW-A2 plan 2B-1 adds design §4.2 changes 1-7 here, one entry each. The upstream `WebSocket`
class and the blocking `EspTcp::Connect()` are left as they are (the FW-A protocol and `HttpClient`
use them); the changes are new entry points.

1. **WebSocket frame parts (change 6, part):** `include/ws_frame.h`, `src/ws_frame.cc`. Pure
   (no FreeRTOS, no sockets; host-tested in `firmware/host_test/test_ws_frame.cc`): a decoder of
   the server's frames in any split (fragments joined, control frames passed out as they come,
   broken frames and lengths over a limit are errors that stay), a masked client-frame encoder and
   the opening handshake (request, response). Nothing answers a ping here: the link does it
   through its send queue.
