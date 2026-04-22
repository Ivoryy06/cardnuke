#include "cardnuke.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>

bool is_windows() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm* tm = std::localtime(&t);
    std::ostringstream ss;
    ss << std::put_time(tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void log(const std::string& msg, const std::string& fn) {
    std::string line = "[" + timestamp() + "] " + msg;
    std::cout << line << "\n";
    if (!fn.empty()) {
        std::ofstream f(fn, std::ios::app);
        if (f) f << line << "\n";
    }
}