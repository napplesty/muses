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

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <iostream>
#include <sstream>
#include <map>
#include <filesystem>
#include "muses/logging.hpp"


#ifndef _NET_DRIVER_HPP
#define _NET_DRIVER_HPP

namespace muses {

class HttpInfo {
public:
    std::string method;
    std::string url;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string body;
};

class HttpContext {
public:
    static const int BUFFER_SIZE = 1024;

    static HttpInfo parse_request(int client_fd) {
        HttpInfo http_info;
        char buffer[BUFFER_SIZE*4];
        std::string request;
        int bytes_read;
        while ((bytes_read = read(client_fd, buffer, BUFFER_SIZE - 1)) > 0) {
            buffer[bytes_read] = '\0';
            request += buffer;
            if (request.find("\n") != std::string::npos) {
                break;
            }
            
        }
        std::istringstream request_stream(request);
        std::getline(request_stream, http_info.method, ' ');
        std::getline(request_stream, http_info.url, ' ');
        std::getline(request_stream, http_info.version, '\n');
        std::string header_line;
        while (std::getline(request_stream, header_line) && header_line != "\r") {
            size_t delimiter_pos = header_line.find(": ");
            if (delimiter_pos != std::string::npos) {
                std::string key = header_line.substr(0, delimiter_pos);
                std::string value = header_line.substr(delimiter_pos + 2);
                http_info.headers[key] = value;
            }
        }
        http_info.body = std::string(buffer + request_stream.tellg(), request.size() - request_stream.tellg());
        return http_info;
    }

    static std::string read_file(const std::string& file_path) {
        std::ifstream file(file_path);
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return content;
    }

    static ssize_t get_file_size(const std::string &filename) {
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        return file.tellg();
    }

    static std::string send_response(const HttpInfo &http_info, int socket_fd) {
        std::stringstream ss;
        ss << "HTTP/1.1 200 OK" << std::endl;
        ss << "Content-Type: text/html; charset=utf-8" << std::endl;
        ss << "Connection: close" << std::endl;
        if(http_info.url == "/"){
            ss << "Content-Length: " << get_file_size("./statics/index.html") << std::endl;
            ss << std::endl;
            ss << read_file("./statics/index.html") << std::endl;
        } else {
            ss << "Content-Length: " << get_file_size("./statics" + http_info.url) << std::endl;
            ss << std::endl;
            ss << read_file("./statics" + http_info.url) << std::endl;
        }
        std::string response = ss.str();
        return response;
    }

    static std::string send_404_response(const HttpInfo &http_info, int socket_fd) {
        std::stringstream ss;
        ss << "HTTP/1.1 404 Not Found" << std::endl;
        ss << "Content-Type: text/html; charset=utf-8" << std::endl;
        ss << "Connection: close" << std::endl;
        ss << "Content-Length: " << get_file_size("./statics/404.html") << std::endl;
        ss << std::endl;

        ss << read_file("./statics/404.html") << std::endl;
        std::string response = ss.str();
        return response;
    }

    static std::string send_405_response(const HttpInfo &http_info, int socket_fd) {
        std::stringstream ss;
        ss << "HTTP/1.1 405 Method Not Allowed" << std::endl;
        ss << "Content-Type: text/html; charset=utf-8" << std::endl;
        ss << "Connection: close" << std::endl;
        ss << "Content-Length: " << get_file_size("./statics/405.html") << std::endl;
        ss << std::endl;

        ss << read_file("./statics/405.html") << std::endl;
        std::string response = ss.str();
        return response;
    }

    static bool handle_request(int socket_fd) {
        HttpInfo http_info = parse_request(socket_fd);
        std::string response;
        bool ret = true;

        if (http_info.method == "GET") {
            std::string file_path = "./statics" + http_info.url.substr(http_info.url.find('/') + 1);
            if (std::filesystem::exists(file_path)) {
                response = send_response(http_info, socket_fd);
                ret = false;
            } else {
                response = send_404_response(http_info, socket_fd);
                ret = true;
                MUSES_INFO(std::string("404: request:") + http_info.url);
            }
        } else {
            response = send_405_response(http_info, socket_fd);
            ret = true;
            MUSES_INFO(std::string("405: method:") + http_info.method);
        }
        send(socket_fd, response.c_str(), response.size(), 0);
        return ret;
    }
};

}; // namespace muses

#endif