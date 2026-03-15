cmake_minimum_required(VERSION 3.10)
project(exposhot_camera)
set(CMAKE_CXX_STANDARD 17)
add_definitions(-DLIBRAW_BUILDLIB -D_ENABLE_EXTENDED_ALIGNED_STORAGE)
if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()


set(OPENCV_PATH ${CMAKE_CURRENT_SOURCE_DIR}/third_party/opencv/${OHOS_ARCH})

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/camera
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libraw/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/motion_stack/include
    ${OPENCV_PATH}/include/opencv4
)

file(GLOB_RECURSE SOURCES "*.cpp" "*.cxx" "*.cc" "src/*.cpp" "src/**/*.cpp")
add_library(expocamera SHARED ${SOURCES}
    napi_expo_camera.cpp
    camera/expo_camera.cpp
    camera/task_queue.cpp
    camera/image_processor.cpp
    camera/burst_capture.cpp
    camera/file_saver.cpp
)


find_library(
    # Sets the name of the path variable.
    hilog-lib
    # Specifies the name of the NDK library that
    # you want CMake to locate.
    hilog_ndk.z
)

find_library(
    # Sets the name of the path variable.
    libace-lib
    # Specifies the name of the NDK library that
    # you want CMake to locate.
    ace_ndk.z
)

find_library(
    # Sets the name of the path variable.
    libnapi-lib
    # Specifies the name of the NDK library that
    # you want CMake to locate.
    ace_napi.z
)

find_library(
    # Sets the name of the path variable.
    libuv-lib
    # Specifies the name of the NDK library that
    # you want CMake to locate.
    uv
)

target_link_libraries(expocamera PUBLIC
    ${hilog-lib} ${libace-lib} ${libnapi-lib} ${libuv-lib}
     )

target_link_libraries(expocamera PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    libohcamera.so
    libohimage.so
    libimage_receiver.so
    libnative_image.so
    libnative_buffer.so
    ${OPENCV_PATH}/lib/libopencv_calib3d.so.408
    ${OPENCV_PATH}/lib/libopencv_core.so.408
    ${OPENCV_PATH}/lib/libopencv_dnn.so.408
    ${OPENCV_PATH}/lib/libopencv_features2d.so.408
    ${OPENCV_PATH}/lib/libopencv_flann.so.408
    ${OPENCV_PATH}/lib/libopencv_gapi.so.408
    ${OPENCV_PATH}/lib/libopencv_highgui.so.408
    ${OPENCV_PATH}/lib/libopencv_imgcodecs.so.408
    ${OPENCV_PATH}/lib/libopencv_imgproc.so.408
    ${OPENCV_PATH}/lib/libopencv_ml.so.408
    ${OPENCV_PATH}/lib/libopencv_objdetect.so.408
    ${OPENCV_PATH}/lib/libopencv_photo.so.408
    ${OPENCV_PATH}/lib/libopencv_stitching.so.408
    ${OPENCV_PATH}/lib/libopencv_video.so.408
    ${OPENCV_PATH}/lib/libopencv_videoio.so.408
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/motion_stack/lib/libmotion_stack.a
)
