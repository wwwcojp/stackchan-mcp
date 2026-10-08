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
2. **Socket steps with deadlines (changes 1-3):** `include/sock_slice.h`, `src/sock_slice.cc`. POSIX
   calls only (lwIP on the device, Linux loopback in `firmware/host_test/test_sock_slice.cc`): a
   connect to a dotted IPv4 address within a deadline (non-blocking `connect()` and `select()` in
   200 ms slices that look at a stop request), one send and one receive within a timeout
   (`SO_SNDTIMEO` / `SO_RCVTIMEO`; under 1 ms is refused, as lwIP takes 0 as "forever").
3. **`EspTcp` managed links (changes 1-5):** `src/esp/esp_tcp.{h,cc}`. New entry points beside the
   unchanged `Connect()` / `Disconnect()` / `Send()`: `ConnectManaged` (change 1, no receive task
   yet), `ReceiveSlice` (the handshake before the task), `StartReceive` with its own name, priority
   and stack (changes 2 and 5: 200 ms receive slices that look at the stop request; a passive end
   shuts the socket down and keeps the descriptor; `on_exit` runs last on the task), `SendSlice`
   (change 3: the caller cuts the deadline with `firmware/main/net/send_deadline`), `RequestStop` /
   `WaitStopped` (change 4). A managed object closes its socket only in its destructor, which the
   owner calls after the receive task ended (change 4). `esp_tcp.h` moved from `src/esp/` to
   `include/` so the FW's links can use these entry points.
