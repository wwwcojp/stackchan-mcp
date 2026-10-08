cmake_minimum_required(VERSION 3.16)

project(stackchan_firmware_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(mdns_candidate_extract_test
    test_mdns_candidate_extract.cc
    ../main/protocols/mdns_candidate_extract.cc
)

target_compile_definitions(mdns_candidate_extract_test
    PRIVATE STACKCHAN_MDNS_HOST_TEST
)

target_include_directories(mdns_candidate_extract_test
    PRIVATE
        stubs
        ../main/protocols
)

target_link_libraries(mdns_candidate_extract_test
    PRIVATE GTest::gtest_main
)

add_executable(listening_profile_test
    test_listening_profile.cc
    ../main/listening_profile.cc
)

target_include_directories(listening_profile_test
    PRIVATE
        ../main
)

target_link_libraries(listening_profile_test
    PRIVATE GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(mdns_candidate_extract_test)
gtest_discover_tests(listening_profile_test)

# StackChan FW-A: cJSON for host tests. Prefer the copy shipped with ESP-IDF
# (espressif/idf image sets IDF_PATH); fall back to FetchContent otherwise.
enable_language(C)  # cJSON is C; the project above enables CXX only
if(DEFINED ENV{IDF_PATH} AND EXISTS "$ENV{IDF_PATH}/components/json/cJSON/cJSON.c")
    add_library(cjson STATIC "$ENV{IDF_PATH}/components/json/cJSON/cJSON.c")
    target_include_directories(cjson PUBLIC "$ENV{IDF_PATH}/components/json/cJSON")
else()
    FetchContent_Declare(
        cjson
        GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
        GIT_TAG v1.7.18
    )
    FetchContent_MakeAvailable(cjson)
endif()

# StackChan FW-A T0: the host build links cJSON (design §5).
add_executable(cjson_link_test test_cjson_link.cc)
target_link_libraries(cjson_link_test PRIVATE GTest::gtest_main cjson)
add_test(NAME cjson_link_test COMMAND cjson_link_test)

add_executable(message_stamp_test
    test_message_stamp.cc
    ../main/protocols/message_stamp.cc
)
target_include_directories(message_stamp_test PRIVATE ../main/protocols)
target_link_libraries(message_stamp_test PRIVATE GTest::gtest_main cjson)
add_test(NAME message_stamp_test COMMAND message_stamp_test)

add_executable(abort_policy_test
    test_abort_policy.cc
    ../main/abort_policy.cc
)
target_include_directories(abort_policy_test PRIVATE ../main)
target_link_libraries(abort_policy_test PRIVATE GTest::gtest_main cjson)
add_test(NAME abort_policy_test COMMAND abort_policy_test)

add_executable(server_time_test
    test_server_time.cc
    ../main/server_time.cc
)
target_include_directories(server_time_test PRIVATE ../main)
target_link_libraries(server_time_test PRIVATE GTest::gtest_main cjson)
add_test(NAME server_time_test COMMAND server_time_test)

# StackChan FW-A2 §2.1-2.3, §7.1-1: the playback gate core and the contract vectors
add_library(playback_gate_core STATIC ../main/gate/playback_gate_core.cc)
target_include_directories(playback_gate_core PUBLIC ../main/gate)

add_executable(playback_gate_core_test test_playback_gate_core.cc)
target_link_libraries(playback_gate_core_test PRIVATE GTest::gtest_main playback_gate_core)
add_test(NAME playback_gate_core_test COMMAND playback_gate_core_test)

add_executable(playback_gate_vectors_test test_playback_gate_vectors.cc)
target_compile_definitions(playback_gate_vectors_test
    PRIVATE CONTRACT_VECTORS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/contract_vectors")
target_link_libraries(playback_gate_vectors_test PRIVATE GTest::gtest_main playback_gate_core cjson)
add_test(NAME playback_gate_vectors_test COMMAND playback_gate_vectors_test)

# StackChan FW-A2 §2.2, §6.1, §7.1-2: origin of queued audio and the queue-model enumeration
add_executable(audio_origin_test test_audio_origin.cc ../main/audio/audio_origin.cc)
target_include_directories(audio_origin_test PRIVATE ../main/audio)
target_link_libraries(audio_origin_test PRIVATE GTest::gtest_main playback_gate_core)
add_test(NAME audio_origin_test COMMAND audio_origin_test)

# StackChan FW-A2 §2.4: UiController Step
add_library(ui_controller_core STATIC ../main/ui/ui_controller_core.cc)
target_include_directories(ui_controller_core PUBLIC ../main/ui)
add_executable(ui_controller_core_test test_ui_controller_core.cc)
target_link_libraries(ui_controller_core_test PRIVATE GTest::gtest_main ui_controller_core)
add_test(NAME ui_controller_core_test COMMAND ui_controller_core_test)

# StackChan FW-A2 §7.1-5: replay the TLA+ model traces through Step
add_executable(ui_model_traces_test test_ui_model_traces.cc)
target_compile_definitions(ui_model_traces_test PRIVATE MODEL_TRACES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/model_traces")
target_link_libraries(ui_model_traces_test PRIVATE GTest::gtest_main ui_controller_core cjson)
add_test(NAME ui_model_traces_test COMMAND ui_model_traces_test)

# StackChan FW-A2 §3: the link manager's transition function
add_executable(link_manager_core_test test_link_manager_core.cc ../main/link/link_manager_core.cc)
target_include_directories(link_manager_core_test PRIVATE ../main/link)
target_link_libraries(link_manager_core_test PRIVATE GTest::gtest_main)
add_test(NAME link_manager_core_test COMMAND link_manager_core_test)

# StackChan FW-A2 §4.1: SendAll under one deadline
add_executable(send_deadline_test test_send_deadline.cc ../main/net/send_deadline.cc)
target_include_directories(send_deadline_test PRIVATE ../main/net)
target_link_libraries(send_deadline_test PRIVATE GTest::gtest_main)
add_test(NAME send_deadline_test COMMAND send_deadline_test)

# StackChan FW-A2 plan 2A: the contract's JSON messages
add_executable(wire_test test_wire.cc ../main/link/wire.cc)
target_include_directories(wire_test PRIVATE ../main/link)
target_link_libraries(wire_test PRIVATE GTest::gtest_main cjson)
add_test(NAME wire_test COMMAND wire_test)

# StackChan FW-A2 plan 2A: the send queue (design §4.1)
add_executable(send_queue_test test_send_queue.cc ../main/net/send_queue.cc)
target_include_directories(send_queue_test PRIVATE ../main/net)
target_link_libraries(send_queue_test PRIVATE GTest::gtest_main)
add_test(NAME send_queue_test COMMAND send_queue_test)

# StackChan FW-A2 plan 2A: the AudioService's bookkeeping and the split-pipeline enumeration
add_executable(playback_book_test test_playback_book.cc ../main/audio/playback_book.cc ../main/audio/audio_origin.cc)
target_include_directories(playback_book_test PRIVATE ../main/audio)
target_link_libraries(playback_book_test PRIVATE GTest::gtest_main playback_gate_core)
add_test(NAME playback_book_test COMMAND playback_book_test)

# StackChan FW-A2 plan 2A: the playback gate shell (the vectors through it, queue failures, R5)
add_executable(playback_gate_test test_playback_gate.cc ../main/gate/playback_gate.cc
    ../main/net/send_queue.cc ../main/link/wire.cc)
target_compile_definitions(playback_gate_test
    PRIVATE CONTRACT_VECTORS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/contract_vectors")
target_include_directories(playback_gate_test PRIVATE ../main/gate ../main/net ../main/link)
target_link_libraries(playback_gate_test PRIVATE GTest::gtest_main playback_gate_core cjson)
add_test(NAME playback_gate_test COMMAND playback_gate_test)

# StackChan FW-A2 plan 2A: the link manager shell (notice queue, tick, ports) and link phases
add_executable(link_manager_test test_link_manager.cc ../main/link/link_manager.cc ../main/link/link_manager_core.cc)
target_include_directories(link_manager_test PRIVATE ../main/link)
target_link_libraries(link_manager_test PRIVATE GTest::gtest_main)
add_test(NAME link_manager_test COMMAND link_manager_test)

# StackChan FW-A2 plan 2A: the UiController shell (fake ports)
add_executable(ui_controller_test test_ui_controller.cc ../main/ui/ui_controller.cc)
target_include_directories(ui_controller_test PRIVATE ../main ../main/ui ../main/gate ../main/net ../main/link)
target_link_libraries(ui_controller_test PRIVATE GTest::gtest_main ui_controller_core)
add_test(NAME ui_controller_test COMMAND ui_controller_test)

# StackChan FW-A2 plan 2A: the gateway candidates and the ws://IPv4 check (design §3.6)
add_executable(gateway_target_test test_gateway_target.cc ../main/link/gateway_target.cc)
target_include_directories(gateway_target_test PRIVATE ../main/link)
target_link_libraries(gateway_target_test PRIVATE GTest::gtest_main)
add_test(NAME gateway_target_test COMMAND gateway_target_test)

# StackChan FW-A2 plan 2B-1: the WebSocket frame parts of the vendored esp-ml307 (design §4.2 change 6)
add_executable(ws_frame_test test_ws_frame.cc ../components/78__esp-ml307/src/ws_frame.cc)
target_include_directories(ws_frame_test PRIVATE ../components/78__esp-ml307/include)
target_link_libraries(ws_frame_test PRIVATE GTest::gtest_main)
add_test(NAME ws_frame_test COMMAND ws_frame_test)

# StackChan FW-A2 plan 2B-1: socket steps with deadlines of the vendored esp-ml307 (design §4.2 changes 1-3)
add_executable(sock_slice_test test_sock_slice.cc ../components/78__esp-ml307/src/sock_slice.cc)
target_include_directories(sock_slice_test PRIVATE ../components/78__esp-ml307/include)
target_link_libraries(sock_slice_test PRIVATE GTest::gtest_main)
add_test(NAME sock_slice_test COMMAND sock_slice_test)

# StackChan FW-A2 plan 2B-1: what the receive tasks decide (audio frames, the hello reply, routing)
add_executable(rx_route_test test_rx_route.cc ../main/link/rx_route.cc ../main/link/wire.cc)
target_include_directories(rx_route_test PRIVATE ../main/link)
target_link_libraries(rx_route_test PRIVATE GTest::gtest_main cjson)
add_test(NAME rx_route_test COMMAND rx_route_test)
