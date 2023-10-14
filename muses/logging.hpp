// MIT License

// Copyright (c) 2023 nastyapple

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#ifndef _LOGGING_HPP
#define _LOGGING_HPP

#include <iostream>
#include <mutex>
#include <fstream>
#include <condition_variable>
#include <thread>
#include <muses/queue.hpp>
#include <ctime>
#include <chrono>
#include <functional>
#include <string>
#include <tuple>
#include <cstring>
#include <sstream>

#ifndef MUSES_LOG_LEVEL
#define MUSES_LOG_LEVEL LogLevel::Debug
#endif

#ifndef MUSES_LOG_FILENAME
#define MUSES_LOG_FILENAME "log.txt"
#endif

namespace muses {

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error,
    Fatal
};

class Logger {
private:
    Logger(LogLevel level, std::string log_filename)
    : level(level), is_running(true), 
    write_buffer(), log_filename(log_filename), 
    thread(&Logger::write_to, this){}

    ~Logger() {
        {
            std::unique_lock<std::mutex> lock(mutex);
            is_running = false;
            cv.notify_all();
        }
        thread.join();
    }

    std::string get_level_string(LogLevel level) {
        if(level == LogLevel::Debug) {
            return "Debug";
        } else if (level == LogLevel::Info) {
            return "Info";
        } else if (level == LogLevel::Warning) {
            return "Warning";
        } else if (level == LogLevel::Error) {
            return "Error";
        } else {
            return "Fatal";
        }
    }

    void write_to() {
        while (true) {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock,[this]{return (!this->msg_queue.empty()) || !this->is_running;});
            {
                std::stringstream ss;
                while(!msg_queue.empty()) {
                    std::tuple<LogLevel, std::string, std::string, std::time_t> msg;
                    msg_queue.wait_and_pop(msg);
                    char time_string[50];
                    memset(time_string, '\0', sizeof(time_string));
                    std::strftime(time_string, sizeof(time_string), "%Y-%m-%d %H:%M:%S", std::localtime(&std::get<3>(msg)));
                    std::string time_str(time_string);
                    std::string level_str = get_level_string(std::get<0>(msg));
                    ss << '[' << level_str << "] " << time_string << ' ' << std::get<1>(msg) << ": " << std::get<2>(msg) << std::endl;
                }
                write_buffer += ss.str();
                ss.str("");
                ss.clear();
            }
            if(write_buffer.size() >= 4096 || !is_running) {
                std::ofstream file(log_filename, std::ios::out|std::ios::app);
                file << write_buffer;
                file.close();
                write_buffer.clear();
            }
            if(!is_running) {
                break;
            }
        }
    }

public:
    static Logger& get_instance() {
        static Logger logger(MUSES_LOG_LEVEL, MUSES_LOG_FILENAME);
        return logger;
    }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void log(LogLevel level, const std::string &message, const std::string &func_name) {
        if (level < this->level) {
            return;
        } else {
            std::time_t timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            msg_queue.push(std::make_tuple(level, func_name, message, timestamp));
        }
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
    std::string write_buffer;
    std::thread thread;
    std::string log_filename;
    bool is_running;
    LogLevel level;
    ThreadSafeQueue<std::tuple<LogLevel, std::string, std::string, std::time_t> > msg_queue;
};
};

#define MUSES_DEBUG(msg) muses::Logger::get_instance().log(muses::LogLevel::Debug,msg,__func__)
#define MUSES_INFO(msg) muses::Logger::get_instance().log(muses::LogLevel::Info,msg,__func__)
#define MUSES_WARNING(msg) muses::Logger::get_instance().log(muses::LogLevel::Warning,msg,__func__)
#define MUSES_ERROR(msg) muses::Logger::get_instance().log(muses::LogLevel::Error,msg,__func__)
#define MUSES_FATAL(msg) muses::Logger::get_instance().log(muses::LogLevel::Fatal,msg,__func__)

#endif