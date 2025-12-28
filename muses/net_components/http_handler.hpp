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
#include <fstream>
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
        char buffer[BUFFER_SIZE];
        std::string request;
        const size_t MAX_REQUEST_SIZE = BUFFER_SIZE * 8;
        int bytes_read;

        while ((bytes_read = read(client_fd, buffer, BUFFER_SIZE - 1)) > 0) {
            buffer[bytes_read] = '\0';
            request.append(buffer, bytes_read);
            if (request.find("\r\n\r\n") != std::string::npos) {
                break;
            }
            if (request.size() > MAX_REQUEST_SIZE) {
                break;
            }
        }

        std::istringstream request_stream(request);
        std::getline(request_stream, http_info.method, ' ');
        std::getline(request_stream, http_info.url, ' ');
        std::getline(request_stream, http_info.version, '\r');
        std::string header_line;
        while (std::getline(request_stream, header_line) && header_line != "\r") {
            size_t delimiter_pos = header_line.find(": ");
            if (delimiter_pos != std::string::npos) {
                std::string key = header_line.substr(0, delimiter_pos);
                std::string value = header_line.substr(delimiter_pos + 2);
                http_info.headers[key] = value;
            }
        }

        return http_info;
    }

    static std::string read_file(const std::string& file_path) {
        std::ifstream file(file_path);
        if (!file.is_open()) {
            return "";
        }
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return content;
    }

    static ssize_t get_file_size(const std::string &filename) {
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return -1;
        }
        return file.tellg();
    }

    static std::string get_content_type(const std::string& file_path) {
        size_t dot_pos = file_path.find_last_of('.');
        if (dot_pos == std::string::npos) {
            return "application/octet-stream";
        }
        std::string ext = file_path.substr(dot_pos);
        if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
        if (ext == ".css") return "text/css; charset=utf-8";
        if (ext == ".js") return "application/javascript; charset=utf-8";
        if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
        if (ext == ".png") return "image/png";
        if (ext == ".gif") return "image/gif";
        if (ext == ".svg") return "image/svg+xml";
        if (ext == ".json") return "application/json";
        if (ext == ".txt") return "text/plain; charset=utf-8";
        return "application/octet-stream";
    }

    static std::string send_response(const std::string& file_path) {
        std::string content = read_file(file_path);
        std::stringstream ss;

        if (content.empty()) {
            ss << "HTTP/1.1 500 Internal Server Error" << std::endl;
            ss << "Content-Type: text/plain" << std::endl;
            ss << "Connection: close" << std::endl;
            ss << "Content-Length: 13" << std::endl;
            ss << std::endl;
            ss << "500 Error" << std::endl;
            return ss.str();
        }

        ss << "HTTP/1.1 200 OK" << std::endl;
        ss << "Content-Type: " << get_content_type(file_path) << std::endl;
        ss << "Connection: close" << std::endl;
        ss << "Content-Length: " << content.size() << std::endl;
        ss << std::endl;
        ss << content;
        return ss.str();
    }

    static std::string send_404_response(const HttpInfo &http_info, int socket_fd) {
        std::string content = read_file("./statics/404.html");
        std::stringstream ss;
        ss << "HTTP/1.1 404 Not Found" << std::endl;
        ss << "Content-Type: text/html; charset=utf-8" << std::endl;
        ss << "Connection: close" << std::endl;
        if (content.empty()) {
            ss << "Content-Length: 9" << std::endl;
            ss << std::endl;
            ss << "404 Error";
        } else {
            ss << "Content-Length: " << content.size() << std::endl;
            ss << std::endl;
            ss << content;
        }
        return ss.str();
    }

    static std::string send_405_response(const HttpInfo &http_info, int socket_fd) {
        std::string content = read_file("./statics/405.html");
        std::stringstream ss;
        ss << "HTTP/1.1 405 Method Not Allowed" << std::endl;
        ss << "Content-Type: text/html; charset=utf-8" << std::endl;
        ss << "Connection: close" << std::endl;
        if (content.empty()) {
            ss << "Content-Length: 9" << std::endl;
            ss << std::endl;
            ss << "405 Error";
        } else {
            ss << "Content-Length: " << content.size() << std::endl;
            ss << std::endl;
            ss << content;
        }
        return ss.str();
    }

    static bool handle_request(int socket_fd) {
        HttpInfo http_info = parse_request(socket_fd);
        std::string response;

        MUSES_INFO(std::string("Request: ") + http_info.method + " " + http_info.url);

        if (http_info.method != "GET") {
            response = send_405_response(http_info, socket_fd);
            MUSES_INFO(std::string("Response: 405"));
        } else {
            std::string file_path = "./statics" + http_info.url;

            if (http_info.url == "/") {
                file_path = "./statics/index.html";
            }

            std::string normalized_path;
            try {
                normalized_path = std::filesystem::canonical(file_path);
                std::string statics_path = std::filesystem::canonical("./statics");

                if (normalized_path.find(statics_path) != 0) {
                    response = send_404_response(http_info, socket_fd);
                    MUSES_INFO(std::string("Response: 404 (path traversal blocked)"));
                } else if (std::filesystem::exists(file_path)) {
                    response = send_response(file_path);
                    MUSES_INFO(std::string("Response: 200"));
                } else {
                    response = send_404_response(http_info, socket_fd);
                    MUSES_INFO(std::string("Response: 404"));
                }
            } catch (const std::exception& e) {
                response = send_404_response(http_info, socket_fd);
                MUSES_INFO(std::string("Response: 404 (filesystem error)"));
            }
        }

        size_t total_sent = 0;
        size_t response_size = response.size();
        while (total_sent < response_size) {
            ssize_t sent = send(socket_fd, response.c_str() + total_sent,
                               response_size - total_sent, 0);
            if (sent < 0) {
                break;
            }
            if (sent == 0) {
                break;
            }
            total_sent += sent;
        }

        return true;
    }
};

}; // namespace muses

#endif