CXX = g++
CXXFLAGS = -O2 -Wall -Wextra -std=c++17

LINUX_TARGET = cardnuke
WIN_TARGET = cardnuke_win.exe
PREFIX = /usr/local/bin

linux: $(LINUX_TARGET)

$(LINUX_TARGET): cardnuke.cpp
	$(CXX) $(CXXFLAGS) -o $@ cardnuke.cpp

win: $(WIN_TARGET)

$(WIN_TARGET): cardnuke_win.cpp
	$(CXX) $(CXXFLAGS) -o $@ cardnuke_win.cpp -lws2_32 -lsetupapi

# Runs the safety tests on disk image files. No real card is touched.
test: $(LINUX_TARGET)
	BIN=./$(LINUX_TARGET) bash tests/run-tests.sh

install: $(LINUX_TARGET)
	cp $(LINUX_TARGET) $(PREFIX)/

clean:
	rm -f $(LINUX_TARGET) $(WIN_TARGET) *.o

.PHONY: all linux win test install clean
