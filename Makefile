CXX ?= g++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra -pthread -Iinclude
LDFLAGS ?= -lasound -lpthread
ifneq ($(shell which pkg-config 2>/dev/null),)
    ifeq ($(shell pkg-config --exists libgpiod && echo yes),yes)
        LDFLAGS += -lgpiod
    endif
endif

TARGET = looper
TEST_TARGET = looper_tests

SRCS = src/main.cpp src/audio_device.cpp src/looper_engine.cpp src/input_manager.cpp src/gpio_manager.cpp src/wav_file.cpp src/wav_worker.cpp src/latency_calibrator.cpp
OBJS = $(SRCS:.cpp=.o)

TEST_SRCS = test/test_suite.cpp src/looper_engine.cpp src/wav_file.cpp src/wav_worker.cpp src/latency_calibrator.cpp

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -UNDEBUG -o $@ $(filter %.cpp %.o,$^) $(LDFLAGS)

$(TEST_TARGET): $(TEST_SRCS) $(wildcard include/*.hpp)
	$(CXX) $(CXXFLAGS) -UNDEBUG -o $@ $(filter %.cpp %.o,$^) $(LDFLAGS)

test: $(TEST_TARGET) looper_regressions
	./$(TEST_TARGET)
	./looper_regressions

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) looper_regressions $(TARGET) $(TEST_TARGET) test_44k.wav test_48k.wav

.PHONY: all clean test

.DEFAULT_GOAL := all
-include $(OBJS:.o=.d)

looper_regressions: test/regression.cpp src/looper_engine.cpp src/wav_file.cpp src/wav_worker.cpp $(wildcard include/*.hpp)
	$(CXX) $(CXXFLAGS) -UNDEBUG -o $@ $(filter %.cpp,$^) $(LDFLAGS)
