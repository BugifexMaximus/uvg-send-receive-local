cmake_minimum_required(VERSION 3.16)
project(uvgrtp_opencv_sender CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(UVGRTP_DISABLE_TESTS ON CACHE BOOL "" FORCE)
set(UVGRTP_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(UVGRTP_DISABLE_INSTALL ON CACHE BOOL "" FORCE)

add_subdirectory(uvgRTP)

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV REQUIRED libavcodec libavutil libswscale)

add_executable(opencv_sender examples/opencv_sender.cc)
target_include_directories(opencv_sender PRIVATE ${OpenCV_INCLUDE_DIRS} ${LIBAV_INCLUDE_DIRS})
target_compile_options(opencv_sender PRIVATE ${LIBAV_CFLAGS_OTHER})
if (LIBAV_LDFLAGS_OTHER)
    target_link_options(opencv_sender PRIVATE ${LIBAV_LDFLAGS_OTHER})
endif()
target_link_libraries(opencv_sender PRIVATE uvgrtp ${OpenCV_LIBS} ${LIBAV_LIBRARIES})

add_executable(opencv_send_logger examples/opencv_send_logger.cc)
target_include_directories(opencv_send_logger PRIVATE ${OpenCV_INCLUDE_DIRS} ${LIBAV_INCLUDE_DIRS})
target_compile_options(opencv_send_logger PRIVATE ${LIBAV_CFLAGS_OTHER})
if (LIBAV_LDFLAGS_OTHER)
    target_link_options(opencv_send_logger PRIVATE ${LIBAV_LDFLAGS_OTHER})
endif()
target_link_libraries(opencv_send_logger PRIVATE uvgrtp ${OpenCV_LIBS} ${LIBAV_LIBRARIES})

add_executable(simple_receiver examples/simple_receiver.cpp)
target_include_directories(simple_receiver PRIVATE ${LIBAV_INCLUDE_DIRS} ${OpenCV_INCLUDE_DIRS})
target_compile_options(simple_receiver PRIVATE ${LIBAV_CFLAGS_OTHER})
if (LIBAV_LDFLAGS_OTHER)
    target_link_options(simple_receiver PRIVATE ${LIBAV_LDFLAGS_OTHER})
endif()
target_link_libraries(simple_receiver PRIVATE uvgrtp ${LIBAV_LIBRARIES} ${OpenCV_LIBS})
