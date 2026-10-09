
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

find_package(OpenXR REQUIRED CONFIG)
find_library(ANDROID_LIB_EGL EGL)
find_library(ANDROID_LIB_GLESV3 GLESv3)
target_link_libraries(chiaki-jni OpenXR::openxr_loader "${ANDROID_LIB_EGL}" "${ANDROID_LIB_GLESV3}")

find_package (oboe REQUIRED CONFIG)
target_link_libraries(chiaki-jni oboe::oboe)