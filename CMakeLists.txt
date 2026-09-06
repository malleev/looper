cmake_minimum_required(VERSION 3.16)
project(looper VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
if(CMAKE_BUILD_TYPE STREQUAL "ASan")
    set(ENABLE_ASAN ON CACHE BOOL "Enable AddressSanitizer and UndefinedBehaviorSanitizer" FORCE)
endif()
if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "ASan and TSan cannot be enabled together")
endif()

if(ENABLE_ASAN)
    message(STATUS "Enabling AddressSanitizer and UndefinedBehaviorSanitizer")
    add_compile_options(-fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address,undefined,float-cast-overflow)
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
    src/gpio_manager.cpp
    src/display_manager.cpp
    src/wav_file.cpp
    src/wav_worker.cpp
    src/latency_calibrator.cpp
)

add_executable(looper src/main.cpp ${COMMON_SOURCES})
target_link_libraries(looper PRIVATE ${ALSA_LIBRARIES} Threads::Threads)

find_library(GPIOD_LIB gpiod)
if(GPIOD_LIB)
    message(STATUS "Found libgpiod: ${GPIOD_LIB}")
    target_link_libraries(looper PRIVATE ${GPIOD_LIB})
endif()

# Unit & regression tests executable
add_executable(looper_tests
    test/test_suite.cpp
    src/looper_engine.cpp
    src/wav_file.cpp
    src/wav_worker.cpp
    src/latency_calibrator.cpp
)
target_link_libraries(looper_tests PRIVATE ${ALSA_LIBRARIES} Threads::Threads)
target_compile_options(looper_tests PRIVATE -UNDEBUG)

enable_testing()
add_test(NAME unit_tests COMMAND looper_tests)
add_executable(looper_regressions test/regression.cpp src/looper_engine.cpp src/wav_file.cpp src/wav_worker.cpp)
target_link_libraries(looper_regressions PRIVATE Threads::Threads)
target_compile_options(looper_regressions PRIVATE -UNDEBUG)
add_test(NAME mvp_regressions COMMAND looper_regressions)
set_tests_properties(unit_tests mvp_regressions PROPERTIES TIMEOUT 180)
