cmake_minimum_required(VERSION 3.10)
project(exposhot_camera)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(
    ${NATIVERENDER_ROOT_PATH}
    ${NATIVERENDER_ROOT_PATH}/camera
)

add_library(expocamera SHARED
    camera/expo_camera.cpp
    camera/napi_expo_camera.cpp
)

target_link_libraries(expocamera PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    libohcamera.so
    libohimage.so
)
