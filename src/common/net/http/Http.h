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
  size_t headerSize = 0;     // request line + headers including the final "\r\n\r\n", 0 if not complete yet
  size_t contentLength = 0;  // 0 if absent
  bool expect100 = false;    // "Expect: 100-continue" on HTTP/1.1
  std::string_view body;     // exactly contentLength bytes, only set when parseRequest returns 200
};

struct Response {
  int status = 200;
  std::string contentType = "text/plain; charset=utf-8";
  std::string body;
  std::string allow;  // "Allow" header, omitted if empty

  static Response error(int status, std::string body) {
    return Response{.status = status, .body = std::move(body) + "\n"};
  }
};

// parse the request at the beginning of `buf`. returns an HTTP status:
//   0   read more: headers incomplete (`req.headerSize == 0`, unless `bufferFull`), or headers complete and body
//       incomplete (all fields but `body` are filled),
//   200 `req` is filled, extra bytes after the body are ignored,
//   400 malformed request line or Content-Length, 431 headers do not fit (`bufferFull` without "\r\n\r\n"),
//   411 POST without Content-Length, 417 Expect other than 100-continue, 501 Transfer-Encoding.
int parseRequest(std::string_view buf, bool bufferFull, Request &req);

// serialize with Content-Length and "Connection: close".
std::string formatResponse(const Response &rsp);

std::string_view reasonPhrase(int status);

}  // namespace hf3fs::net::http
