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
