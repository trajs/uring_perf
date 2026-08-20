CXX ?= g++
CXXFLAGS ?= -std=c++17 -O3 -march=native -Wall -Wextra -Iinclude -Ithird_party/liburing/src/include -pthread
LDFLAGS ?= third_party/liburing/src/liburing.a -lpthread

TARGET = uring_perf
SRCS = src/main.cpp src/net_utils.cpp src/stats.cpp src/uring_engine.cpp src/worker.cpp
OBJS = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
