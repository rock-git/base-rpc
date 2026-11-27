#pragma once

#include <string>
#include <string_view>

// Minimal HTTP/1.x support for the same-port status pages, see docs/http-sniff.md.
// Only parses and formats bytes: no I/O, no routing.
namespace hf3fs::net::http {

// views into the receive buffer, valid while the buffer is.
struct Request {
  std::string_view method;
  std::string_view target;  // path + optional "?query"
  std::string_view path;
  std::string_view query;  // without '?', empty if absent
  std::string_view version;
  std::string_view headers;  // raw header lines after the request line, not parsed
};

struct Response {
  int status = 200;
  std::string contentType = "text/plain; charset=utf-8";
  std::string body;

  static Response error(int status, std::string body) {
    return Response{.status = status, .body = std::move(body) + "\n"};
  }
};

// parse the request at the beginning of `buf`. returns an HTTP status:
//   0   headers incomplete, read more (unless `bufferFull`),
//   200 `req` is filled,
//   400 malformed request line, 431 headers do not fit (`bufferFull` without "\r\n\r\n").
int parseRequest(std::string_view buf, bool bufferFull, Request &req);

// serialize with Content-Length and "Connection: close".
std::string formatResponse(const Response &rsp);

std::string_view reasonPhrase(int status);

}  // namespace hf3fs::net::http
