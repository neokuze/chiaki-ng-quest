
cmake_minimum_required(VERSION 3.2)

set(CMAKE_CXX_STANDARD 14)

add_library(chiaki-jni SHARED
        src/main/cpp/chiaki-jni.c
        src/main/cpp/log.h
        src/main/cpp/log.c
        src/main/cpp/video-decoder.h
        src/main/cpp/video-decoder.c
        src/main/cpp/audio-decoder.h
        src/main/cpp/audio-decoder.c
        src/main/cpp/audio-output.h
        src/main/cpp/audio-output.cpp
        src/main/cpp/circular-fifo.hpp
        src/main/cpp/xr-renderer.c)
target_link_libraries(chiaki-jni chiaki-lib)

find_library(ANDROID_LIB_LOG log)
find_library(ANDROID_LIB_MEDIANDK mediandk)
find_library(ANDROID_LIB_ANDROID android)
target_link_libraries(chiaki-jni "${ANDROID_LIB_LOG}" "${ANDROID_LIB_MEDIANDK}" "${ANDROID_LIB_ANDROID}")

set(ROOMS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/main/rust/rooms")
find_program(CARGO cargo HINTS "$ENV{HOME}/.cargo/bin")
if(ANDROID_ABI STREQUAL "arm64-v8a" AND CARGO)
    set(ROOMS_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/rooms-target")
    set(ROOMS_LIB "${ROOMS_TARGET_DIR}/aarch64-linux-android/release/librooms.a")
    file(GLOB_RECURSE ROOMS_SOURCES "${ROOMS_DIR}/src/*.rs")
    add_custom_command(OUTPUT "${ROOMS_LIB}"
        COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${ROOMS_TARGET_DIR}
            ${CARGO} build --release --target aarch64-linux-android --manifest-path "${ROOMS_DIR}/Cargo.toml"
        DEPENDS ${ROOMS_SOURCES} "${ROOMS_DIR}/Cargo.toml"
        COMMENT "Building the room engine (Rust)"
        VERBATIM)
    add_custom_target(rooms_rs DEPENDS "${ROOMS_LIB}")
    add_dependencies(chiaki-jni rooms_rs)
    target_link_libraries(chiaki-jni "${ROOMS_LIB}")
    target_compile_definitions(chiaki-jni PRIVATE CHIAKI_ROOMS=1)
else()
    message(STATUS "Room engine disabled (needs cargo and arm64-v8a)")
endif()

find_package(OpenXR REQUIRED CONFIG)
find_library(ANDROID_LIB_EGL EGL)
find_library(ANDROID_LIB_GLESV3 GLESv3)
target_link_libraries(chiaki-jni OpenXR::openxr_loader "${ANDROID_LIB_EGL}" "${ANDROID_LIB_GLESV3}")

find_package (oboe REQUIRED CONFIG)
target_link_libraries(chiaki-jni oboe::oboe)