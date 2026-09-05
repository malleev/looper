CXX ?= g++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra -pthread -Iinclude
LDFLAGS ?= -lasound -lpthread

TARGET = looper
TEST_TARGET = looper_tests

SRCS = src/main.cpp src/audio_device.cpp src/looper_engine.cpp src/input_manager.cpp src/wav_file.cpp src/wav_worker.cpp src/latency_calibrator.cpp
OBJS = $(SRCS:.cpp=.o)

TEST_SRCS = test/test_suite.cpp src/looper_engine.cpp src/wav_file.cpp src/wav_worker.cpp src/latency_calibrator.cpp

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_TARGET): $(TEST_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) $(TEST_TARGET) test_44k.wav test_48k.wav

.PHONY: all clean test
