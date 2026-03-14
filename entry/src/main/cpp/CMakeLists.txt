cmake_minimum_required(VERSION 3.10)
project(exposhot_camera)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(
    ${NATIVERENDER_ROOT_PATH}
    ${NATIVERENDER_ROOT_PATH}/camera
)

add_library(expocamera SHARED
    napi_expo_camera.cpp
    camera/expo_camera.cpp
    camera/task_queue.cpp
    camera/image_processor.cpp
    camera/burst_capture.cpp
    camera/file_saver.cpp
)

target_link_libraries(expocamera PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    libohcamera.so
    libohimage.so
    libimage_receiver.so
    libnative_image.so
    libnative_buffer.so
)
