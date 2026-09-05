cmake_minimum_required(VERSION 3.16)
project(looper VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Wall -Wextra -pthread")

# ALSA dependency
find_package(ALSA REQUIRED)

# Threads
find_package(Threads REQUIRED)

include_directories(include ${ALSA_INCLUDE_DIRS})

set(SOURCES
    src/main.cpp
    src/audio_device.cpp
    src/looper_engine.cpp
    src/input_manager.cpp
    src/wav_file.cpp
    src/wav_worker.cpp
)

add_executable(looper ${SOURCES})
target_link_libraries(looper PRIVATE ${ALSA_LIBRARIES} Threads::Threads)
