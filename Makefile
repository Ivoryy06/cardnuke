CXX = g++
CXXFLAGS = -O2 -Wall -std=c++17

LINUX_TARGET = cardnuke
WIN_TARGET = cardnuke_win.exe
PREFIX = /usr/local/bin

linux: $(LINUX_TARGET)

$(LINUX_TARGET): cardnuke.cpp
	$(CXX) $(CXXFLAGS) -o $@ cardnuke.cpp

win: $(WIN_TARGET)

$(WIN_TARGET): cardnuke_win.cpp
	$(CXX) $(CXXFLAGS) -o $@ cardnuke_win.cpp -lws2_32 -lsetupapi

install: $(LINUX_TARGET)
	cp $(LINUX_TARGET) $(PREFIX)/

clean:
	rm -f $(LINUX_TARGET) $(WIN_TARGET)

.PHONY: all linux win install clean