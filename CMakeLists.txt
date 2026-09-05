cmake_minimum_required(VERSION 3.16)
project(looper VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(ENABLE_ASAN)
    message(STATUS "Enabling AddressSanitizer and UndefinedBehaviorSanitizer")
    add_compile_options(-fsanitize=address,undefined -g -O2)
    add_link_options(-fsanitize=address,undefined)
elseif(ENABLE_TSAN)
    message(STATUS "Enabling ThreadSanitizer")
    add_compile_options(-fsanitize=thread -g -O2)
    add_link_options(-fsanitize=thread)
else()
    add_compile_options(-O3)
endif()

add_compile_options(-Wall -Wextra -pthread)

# ALSA dependency
find_package(ALSA REQUIRED)

# Threads
find_package(Threads REQUIRED)

include_directories(include ${ALSA_INCLUDE_DIRS})

set(COMMON_SOURCES
    src/audio_device.cpp
    src/looper_engine.cpp
    src/input_manager.cpp
    src/wav_file.cpp
    src/wav_worker.cpp
)

add_executable(looper src/main.cpp ${COMMON_SOURCES})
target_link_libraries(looper PRIVATE ${ALSA_LIBRARIES} Threads::Threads)

# Unit & regression tests executable
add_executable(looper_tests
    test/test_suite.cpp
    src/looper_engine.cpp
    src/wav_file.cpp
    src/wav_worker.cpp
)
target_link_libraries(looper_tests PRIVATE Threads::Threads)

enable_testing()
add_test(NAME unit_tests COMMAND looper_tests)
