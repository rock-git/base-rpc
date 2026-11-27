#include "common/net/http/Http.h"

#include <fmt/format.h>

namespace hf3fs::net::http {

int parseRequest(std::string_view buf, bool bufferFull, Request &req) {
  auto end = buf.find("\r\n\r\n");
  if (end == buf.npos) {
    return bufferFull ? 431 : 0;
  }
  auto lineEnd = buf.find("\r\n");
  auto line = buf.substr(0, lineEnd);
  auto sp1 = line.find(' ');
  auto sp2 = sp1 == line.npos ? line.npos : line.find(' ', sp1 + 1);
  if (sp2 == line.npos || line.find(' ', sp2 + 1) != line.npos) {
    return 400;
  }
  req.method = line.substr(0, sp1);
  req.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  req.version = line.substr(sp2 + 1);
  if (req.method.empty() || !req.target.starts_with('/') || !req.version.starts_with("HTTP/1.")) {
    return 400;
  }
  auto q = req.target.find('?');
  req.path = req.target.substr(0, q);
  req.query = q == req.target.npos ? std::string_view{} : req.target.substr(q + 1);
  req.headers = buf.substr(lineEnd + 2, end + 2 - (lineEnd + 2));
  return 200;
}

std::string formatResponse(const Response &rsp) {
  return fmt::format("HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                     rsp.status,
                     reasonPhrase(rsp.status),
                     rsp.contentType,
                     rsp.body.size(),
                     rsp.body);
}

std::string_view reasonPhrase(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 400:
      return "Bad Request";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 431:
      return "Request Header Fields Too Large";
    default:
      return "Unknown";
  }
}

}  // namespace hf3fs::net::http
