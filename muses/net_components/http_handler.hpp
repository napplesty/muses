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

#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "muses/logging.hpp"

#ifndef MUSES_NET_HTTP_HANDLER_HPP
#define MUSES_NET_HTTP_HANDLER_HPP

namespace muses {

// Parsed HTTP request line + headers + body.
class HttpInfo {
public:
    std::string method;
    std::string url;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string body;

    // True if the client requested the connection be kept alive. Defaults to
    // true for HTTP/1.1 unless "Connection: close" is present.
    bool wants_keep_alive() const {
        auto it = headers.find("Connection");
        if (it != headers.end()) {
            return it->second.find("close") == std::string::npos;
        }
        return version == "HTTP/1.1";
    }
};

// A complete HTTP response: the raw bytes and whether the connection should
// be kept alive afterwards. (The reactor adapts this into its own HandlerResult
// when wiring up a request handler.)
struct HttpResponse {
    std::string response;
    bool keep_alive;
};

// Minimal static-file HTTP/1.1 handler. Fixes from the old version:
//   - response lines use CRLF (was \n via std::endl)
//   - files read in binary mode (images were corrupted)
//   - empty files return 200 (were misreported as 500)
//   - path traversal uses filesystem::relative (was fragile prefix match)
//   - handle_request returns keep_alive so the reactor can reuse the socket
class HttpContext {
public:
    static constexpr std::size_t MAX_REQUEST_BYTES = 64 * 1024;

    // Parse a fully-buffered request. Tolerant: a malformed request yields an
    // empty HttpInfo (caller treats as 400).
    static HttpInfo parse_request(const std::string& request) {
        HttpInfo info;
        std::istringstream stream(request);
        std::string line;
        if (!std::getline(stream, info.method, ' ')) return info;
        if (!std::getline(stream, info.url, ' ')) return info;
        std::string ver;
        if (!std::getline(stream, ver, '\r')) return info;
        info.version = ver;
        stream.get();  // consume the trailing '\n'

        while (std::getline(stream, line)) {
            if (line == "\r" || line.empty()) break;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::size_t pos = line.find(": ");
            if (pos == std::string::npos) pos = line.find(':');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string val = line.substr(line[pos + 1] == ' ' ? pos + 2 : pos + 1);
                info.headers[key] = val;
            }
        }
        // Body (if any) is whatever remains.
        std::streampos pos = stream.tellg();
        if (pos >= 0 && static_cast<std::size_t>(pos) < request.size()) {
            info.body = request.substr(static_cast<std::size_t>(pos));
        }
        return info;
    }

    static std::string read_file(const std::string& file_path, bool* exists) {
        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open()) {
            if (exists) *exists = false;
            return "";
        }
        if (exists) *exists = true;
        return std::string((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    }

    static std::string get_content_type(const std::string& file_path) {
        std::size_t dot = file_path.find_last_of('.');
        if (dot == std::string::npos) return "application/octet-stream";
        std::string ext = file_path.substr(dot);
        if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
        if (ext == ".css") return "text/css; charset=utf-8";
        if (ext == ".js")  return "application/javascript; charset=utf-8";
        if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
        if (ext == ".png") return "image/png";
        if (ext == ".gif") return "image/gif";
        if (ext == ".svg") return "image/svg+xml";
        if (ext == ".json") return "application/json";
        if (ext == ".txt") return "text/plain; charset=utf-8";
        return "application/octet-stream";
    }

    // Build a complete HTTP/1.1 response with CRLF line endings.
    static std::string build_response(int status, const std::string& status_text,
                                      const std::string& content_type,
                                      const std::string& body, bool keep_alive) {
        // std::format avoids ostringstream's per-<< virtual dispatch and locale
        // overhead; this runs once per request.
        std::string head = std::format(
            "HTTP/1.1 {} {}\r\n"
            "Content-Type: {}\r\n"
            "Content-Length: {}\r\n"
            "Connection: {}\r\n"
            "\r\n",
            status, status_text, content_type, body.size(),
            keep_alive ? "keep-alive" : "close");
        head += body;
        return head;
    }

    static std::string send_response(const std::string& file_path, bool keep_alive, bool* ok) {
        bool exists = false;
        std::string content = read_file(file_path, &exists);
        if (ok) *ok = exists;
        if (!exists) {
            return build_response(500, "Internal Server Error", "text/plain", "500 Error", false);
        }
        return build_response(200, "OK", get_content_type(file_path), content, keep_alive);
    }

    static std::string send_404(bool keep_alive) {
        bool exists = false;
        std::string content = read_file("./statics/404.html", &exists);
        if (!exists) content = "404 Not Found";
        return build_response(404, "Not Found", "text/html; charset=utf-8", content, keep_alive);
    }

    static std::string send_405(bool keep_alive) {
        bool exists = false;
        std::string content = read_file("./statics/405.html", &exists);
        if (!exists) content = "405 Method Not Allowed";
        return build_response(405, "Method Not Allowed", "text/html; charset=utf-8", content, keep_alive);
    }

    // Why a path could not be resolved to a servable file.
    enum class ResolveError {
        NoStaticsRoot,    // ./statics itself is missing/invalid
        InvalidPath,      // path could not be canonicalized
        TraversalBlocked, // resolved outside the statics root
        NotFound,         // inside root but does not exist
    };

    // Resolve a URL path under the statics root, blocking traversal. Returns
    // the absolute filesystem path on success, or a ResolveError describing
    // the failure. Using std::expected makes every failure mode explicit at
    // the call site (no silent empty-string sentinel).
    static std::expected<std::string, ResolveError> resolve_safe_path(const std::string& url) {
        std::error_code ec;
        namespace fs = std::filesystem;
        fs::path root = fs::canonical("./statics", ec);
        if (ec) return std::unexpected(ResolveError::NoStaticsRoot);
        std::string rel = (url == "/") ? "/index.html" : url;
        fs::path target = fs::weakly_canonical(root / rel.substr(1), ec);
        if (ec) return std::unexpected(ResolveError::InvalidPath);
        // Ensure the resolved target is inside root (no traversal escape).
        auto [root_it, target_it] = std::mismatch(root.begin(), root.end(), target.begin());
        if (root_it != root.end()) return std::unexpected(ResolveError::TraversalBlocked);
        if (!fs::exists(target)) return std::unexpected(ResolveError::NotFound);
        return target.string();
    }

    // Process a complete request buffer. Returns the response bytes and the
    // keep-alive decision. This is what worker threads invoke.
    static HttpResponse handle_request(const std::string& request) {
        HttpInfo info = parse_request(request);
        bool keep_alive = info.wants_keep_alive();

        HttpResponse result;
        if (info.method.empty()) {
            result.response = build_response(400, "Bad Request", "text/plain", "400 Bad Request", false);
            result.keep_alive = false;
            return result;
        }

        if (info.method != "GET") {
            result.response = send_405(keep_alive);
            result.keep_alive = keep_alive;
            MUSES_INFO("Response: 405");
            return result;
        }

        auto resolved = resolve_safe_path(info.url);
        if (!resolved) {
            // All resolve failures surface as 404 to the client (the path is
            // either outside the root, missing, or invalid). The specific
            // ResolveError is logged for diagnosis.
            result.response = send_404(keep_alive);
            result.keep_alive = keep_alive;
            MUSES_INFO("Response: 404");
            return result;
        }

        bool ok = false;
        result.response = send_response(*resolved, keep_alive, &ok);
        result.keep_alive = ok ? keep_alive : false;
        MUSES_INFO("Response: 200");
        return result;
    }
};

}  // namespace muses

#endif  // MUSES_NET_HTTP_HANDLER_HPP
