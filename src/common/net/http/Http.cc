#include "common/net/http/Http.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fmt/format.h>

namespace hf3fs::net::http {
namespace {

bool iequals(std::string_view a, std::string_view b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) { return std::tolower(x) == std::tolower(y); });
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

}  // namespace

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
  req.headerSize = end + 4;

  bool hasLength = false, hasExpect = false, hasTE = false;
  for (auto rest = req.headers; !rest.empty();) {
    auto eol = rest.find("\r\n");
    auto header = rest.substr(0, eol);
    rest.remove_prefix(eol + 2);
    auto colon = header.find(':');
    if (colon == header.npos) {
      continue;
    }
    auto name = header.substr(0, colon);
    auto value = trim(header.substr(colon + 1));
    if (iequals(name, "Content-Length")) {
      size_t n = 0;
      auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
      if (value.empty() || ec != std::errc{} || ptr != value.data() + value.size() || (hasLength && n != req.contentLength)) {
        return 400;
      }
      hasLength = true;
      req.contentLength = n;
    } else if (iequals(name, "Transfer-Encoding")) {
      hasTE = true;
    } else if (iequals(name, "Expect")) {
      if (!iequals(value, "100-continue")) {
        return 417;
      }
      hasExpect = true;
    }
  }
  if (hasTE) {
    return 501;
  }
  if (req.method == "POST" && !hasLength) {
    return 411;
  }
  req.expect100 = hasExpect && req.version == "HTTP/1.1";
  if (buf.size() - req.headerSize < req.contentLength) {
    return 0;
  }
  req.body = buf.substr(req.headerSize, req.contentLength);
  return 200;
}

std::string formatResponse(const Response &rsp) {
  return fmt::format("HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\n{}Connection: close\r\n\r\n{}",
                     rsp.status,
                     reasonPhrase(rsp.status),
                     rsp.contentType,
                     rsp.body.size(),
                     rsp.allow.empty() ? "" : fmt::format("Allow: {}\r\n", rsp.allow),
                     rsp.body);
}

std::string_view reasonPhrase(int status) {
  switch (status) {
    case 100:
      return "Continue";
    case 200:
      return "OK";
    case 400:
      return "Bad Request";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 411:
      return "Length Required";
    case 413:
      return "Payload Too Large";
    case 417:
      return "Expectation Failed";
    case 431:
      return "Request Header Fields Too Large";
    case 501:
      return "Not Implemented";
    default:
      return "Unknown";
  }
}

}  // namespace hf3fs::net::http
