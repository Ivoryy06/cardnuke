CXX = g++
CXXFLAGS = -O2 -Wall -std=c++17
LDFLAGS =

ifeq ($(OS),Windows_NT)
    LDFLAGS += -lws2_32 -lsetupapi
endif

SRCS = main.cpp device.cpp format.cpp health.cpp backup.cpp speed.cpp recover.cpp info.cpp utils.cpp
OBJS = $(SRCS:.cpp=.o)
TARGET = cardnuke

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) speed_test

.PHONY: all clean