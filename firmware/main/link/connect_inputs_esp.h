// StackChan FW-A2 plan 2B-2b (design §3.6): one attempt's connect inputs from NVS and Kconfig, read
// on the connect worker (HubDeps::read_inputs). The same keys as today's OpenAudioChannelInternal
// (websocket_protocol.cc:269-375): websocket.url / fallback_url / token / version, the build-time
// defaults and the force switch; the Device-Id and Client-Id headers. The rules live in the pure
// gateway_target / connect_plan.
#pragma once

#include "connect_plan.h"

namespace stackchan::link {

ConnectInputs ReadConnectInputs();

}  // namespace stackchan::link
