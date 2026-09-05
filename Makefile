CXX ?= g++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra -pthread -Iinclude
LDFLAGS ?= -lasound -lpthread

TARGET = looper
SRCS = src/main.cpp src/audio_device.cpp src/looper_engine.cpp src/input_manager.cpp src/wav_file.cpp src/wav_worker.cpp
OBJS = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
