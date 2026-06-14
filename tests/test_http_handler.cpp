#include <doctest.h>

#include "muses/net_components/http_handler.hpp"

#include <filesystem>
#include <fstream>

namespace {
// Write a temp static file under ./statics and return its relative url path.
std::string write_static(const std::string& name, const std::string& content) {
    std::ofstream f("./statics/" + name, std::ios::binary | std::ios::trunc);
    f << content;
    return "/" + name;
}

void remove_static(const std::string& name) {
    std::error_code ec;
    std::filesystem::remove("./statics/" + name, ec);
}
}  // namespace

TEST_CASE("HttpHandler: GET returns 200 with CRLF headers") {
    auto url = write_static("t_alpha.txt", "hello world");
    auto hr = muses::HttpContext::handle_request(
        "GET " + url + " HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(hr.response.rfind("HTTP/1.1 200", 0) == 0);
    // CRLF required, not bare LF.
    CHECK(hr.response.find("\r\n") != std::string::npos);
    CHECK(hr.response.find("hello world") != std::string::npos);
    CHECK(hr.keep_alive == true);  // HTTP/1.1 default
    remove_static("t_alpha.txt");
}

TEST_CASE("HttpHandler: missing file returns 404") {
    auto hr = muses::HttpContext::handle_request(
        "GET /nope_does_not_exist.txt HTTP/1.1\r\n\r\n");
    CHECK(hr.response.rfind("HTTP/1.1 404", 0) == 0);
}

TEST_CASE("HttpHandler: non-GET returns 405") {
    auto hr = muses::HttpContext::handle_request(
        "POST /index.html HTTP/1.1\r\n\r\n");
    CHECK(hr.response.rfind("HTTP/1.1 405", 0) == 0);
}

TEST_CASE("HttpHandler: path traversal is blocked") {
    // /../etc/passwd must resolve outside statics → 404 (no leak).
    auto hr = muses::HttpContext::handle_request(
        "GET /../../../../etc/passwd HTTP/1.1\r\n\r\n");
    CHECK(hr.response.rfind("HTTP/1.1 404", 0) == 0);
    CHECK(hr.response.find("root:") == std::string::npos);
}

TEST_CASE("HttpHandler: empty file returns 200, not 500") {
    auto url = write_static("t_empty.txt", "");
    auto hr = muses::HttpContext::handle_request(
        "GET " + url + " HTTP/1.1\r\n\r\n");
    CHECK(hr.response.rfind("HTTP/1.1 200", 0) == 0);
    remove_static("t_empty.txt");
}

TEST_CASE("HttpHandler: Connection: close disables keep-alive") {
    auto url = write_static("t_close.txt", "x");
    auto hr = muses::HttpContext::handle_request(
        "GET " + url + " HTTP/1.1\r\nConnection: close\r\n\r\n");
    CHECK(hr.keep_alive == false);
    CHECK(hr.response.find("Connection: close") != std::string::npos);
    remove_static("t_close.txt");
}

TEST_CASE("HttpHandler: parse_request extracts headers") {
    auto info = muses::HttpContext::parse_request(
        "GET /a HTTP/1.1\r\nHost: example.com\r\nUser-Agent: t\r\n\r\n");
    CHECK(info.method == "GET");
    CHECK(info.url == "/a");
    CHECK(info.version == "HTTP/1.1");
    CHECK(info.headers.at("Host") == "example.com");
    CHECK(info.headers.at("User-Agent") == "t");
}
