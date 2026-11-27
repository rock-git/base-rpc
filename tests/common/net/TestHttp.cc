#include <arpa/inet.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "common/net/Client.h"
#include "common/net/Server.h"
#include "common/net/http/Http.h"
#include "common/utils/Address.h"
#include "common/utils/Coroutine.h"
#include "tests/GtestHelpers.h"
#include "tests/common/net/Echo.h"

namespace hf3fs::net::test {
namespace {

class EchoImpl : public serde::ServiceWrapper<EchoImpl, Echo> {
 public:
  CoTryTask<EchoRsp> echo(serde::CallContext &, const EchoReq &req) {
    EchoRsp rsp;
    rsp.val = req.val;
    co_return rsp;
  }
  CoTryTask<HelloRsp> hello(serde::CallContext &, const HelloReq &) { co_return HelloRsp{}; }
  CoTryTask<HelloRsp> fail(serde::CallContext &, const HelloReq &) {
    co_return makeError(RPCCode::kInvalidMessageType);
  }
};

int connectTo(Address addr) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_GE(fd, 0);
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(addr.port);
  sa.sin_addr.s_addr = addr.ip;
  EXPECT_EQ(::connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)), 0);
  return fd;
}

void sendAll(int fd, const std::string &data) {
  EXPECT_EQ(::send(fd, data.data(), data.size(), MSG_NOSIGNAL), ssize_t(data.size()));
}

std::string readToEnd(int fd) {
  std::string out;
  char buf[4096];
  ssize_t n;
  while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
    out.append(buf, n);
  }
  ::close(fd);
  return out;
}

// send `parts` over a blocking socket (pausing between them) and read until the server closes.
std::string rawRequest(Address addr, std::vector<std::string> parts) {
  int fd = connectTo(addr);
  for (auto &part : parts) {
    sendAll(fd, part);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return readToEnd(fd);
}

std::string post(Address addr, std::string_view body, std::string_view path = "/echo") {
  return rawRequest(addr,
                    {fmt::format("POST {} HTTP/1.1\r\nContent-Length: {}\r\n\r\n", path, body.size()) +
                     std::string(body)});
}

std::string get(Address addr, std::string path) { return rawRequest(addr, {"GET " + path + " HTTP/1.1\r\n\r\n"}); }

TEST(TestHttpParser, Parse) {
  http::Request req;
  ASSERT_EQ(http::parseRequest("GET /a?x=1 HTTP/1.1\r\nHost: h\r\n", false, req), 0);
  ASSERT_EQ(http::parseRequest("GET /a?x=1 HTTP/1.1\r\nHost: h\r\n", true, req), 431);
  ASSERT_EQ(http::parseRequest("GET /a?x=1 HTTP/1.1\r\nHost: h\r\n\r\n", false, req), 200);
  ASSERT_EQ(req.method, "GET");
  ASSERT_EQ(req.target, "/a?x=1");
  ASSERT_EQ(req.path, "/a");
  ASSERT_EQ(req.query, "x=1");
  ASSERT_EQ(req.version, "HTTP/1.1");
  ASSERT_EQ(req.headers, "Host: h\r\n");
  ASSERT_EQ(http::parseRequest("GET / HTTP/1.0\r\n\r\n", false, req), 200);
  ASSERT_EQ(req.query, "");
  ASSERT_EQ(req.headers, "");
  for (auto bad : {"hello\r\n\r\n", "GET / HTTP/1.1 x\r\n\r\n", "GET a HTTP/1.1\r\n\r\n", "GET / HTTP/2\r\n\r\n",
                   " / HTTP/1.1\r\n\r\n"}) {
    ASSERT_EQ(http::parseRequest(bad, false, req), 400) << bad;
  }
  ASSERT_EQ(req.headerSize, 18u);
  ASSERT_EQ(req.contentLength, 0u);
  ASSERT_EQ(req.body, "");
  ASSERT_EQ(http::formatResponse(http::Response::error(404, "nope")),
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 5\r\n"
            "Connection: close\r\n\r\nnope\n");
}

TEST(TestHttpParser, Body) {
  http::Request req;
  auto parse = [&](std::string_view s) {
    req = {};
    return http::parseRequest(s, false, req);
  };
  // header names are case insensitive, values are trimmed.
  ASSERT_EQ(parse("POST /a HTTP/1.1\r\ncontent-LENGTH:  5 \r\n\r\nhello!!"), 200);
  ASSERT_EQ(req.contentLength, 5u);
  ASSERT_EQ(req.body, "hello");
  ASSERT_EQ(parse("POST /a HTTP/1.1\r\nContent-Length: 5\r\ncontent-length: 5\r\n\r\nhello"), 200);
  for (auto v : {"", "-1", "+1", "abc", "99999999999999999999", "1 2"}) {
    ASSERT_EQ(parse(fmt::format("POST /a HTTP/1.1\r\nContent-Length: {}\r\n\r\n", v)), 400) << v;
  }
  ASSERT_EQ(parse("POST /a HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n"), 400);
  ASSERT_EQ(parse("POST /a HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"), 501);
  ASSERT_EQ(parse("POST /a HTTP/1.1\r\n\r\n"), 411);
  ASSERT_EQ(parse("GET /a HTTP/1.1\r\n\r\n"), 200);

  ASSERT_EQ(parse("POST /a HTTP/1.1\r\nContent-Length: 1\r\nExpect: 100-Continue\r\n\r\n"), 0);
  ASSERT_TRUE(req.expect100);
  ASSERT_EQ(parse("POST /a HTTP/1.0\r\nContent-Length: 1\r\nexpect: 100-continue\r\n\r\n"), 0);
  ASSERT_FALSE(req.expect100);
  ASSERT_EQ(parse("POST /a HTTP/1.1\r\nContent-Length: 1\r\nExpect: foo\r\n\r\n"), 417);

  ASSERT_EQ(parse("POST /a HTTP/1.1\r\nContent-Length: 5\r\n"), 0);
  ASSERT_EQ(req.headerSize, 0u);
  ASSERT_EQ(parse("POST /a HTTP/1.1\r\nContent-Length: 5\r\n\r\nhel"), 0);
  ASSERT_EQ(req.headerSize, 39u);
  ASSERT_EQ(req.contentLength, 5u);

  http::Response rsp = http::Response::error(405, "x");
  rsp.allow = "GET, HEAD";
  ASSERT_NE(http::formatResponse(rsp).find("Content-Length: 2\r\nAllow: GET, HEAD\r\nConnection"), std::string::npos);
}

class TestHttp : public testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    server_.addSerdeService(std::make_unique<EchoImpl>());
    server_.groups().front()->addHttpRoute("/ping", [](const http::Request &) { return Processor::HttpResponse{.body = "pong"}; });
    server_.groups().front()->addHttpRoute(
        "/echo",
        [](const http::Request &req) {
          return Processor::HttpResponse{.body = fmt::format("{} {}", req.body.size(), req.body.substr(0, 16))};
        },
        true);
    ASSERT_TRUE(server_.setup());
  }

  void TearDown() override {
    client_.stopAndJoin();
    server_.stopAndJoin();
  }

  Address addr() const { return server_.groups().front()->addressList().front(); }

  void checkEcho() {
    client_.start();
    auto ctx = client_.serdeCtx(addr());
    auto result = folly::coro::blockingWait([&]() -> CoTryTask<EchoRsp> {
      Echo<> echo;
      EchoReq req;
      req.val = "foo";
      co_return co_await echo.echo(ctx, req);
    }());
    ASSERT_OK(result);
    ASSERT_EQ(result->val, "foo");
  }

  Server::Config serverConfig_;
  bool initServerConfig_ = [this] {  // read by the Server constructor.
    serverConfig_.groups(0).set_network_type(Address::TCP);
    serverConfig_.groups(0).io_worker().set_read_write_tcp_in_event_thread(GetParam());
    return true;
  }();
  Server server_{serverConfig_};
  Client::Config clientConfig_;
  Client client_{clientConfig_};
};

TEST_P(TestHttp, Serve) {
  serverConfig_.groups(0).processor().set_enable_http(true);
  ASSERT_TRUE(server_.start());

  auto rsp = get(addr(), "/ping?x=1");
  ASSERT_TRUE(rsp.starts_with("HTTP/1.1 200 OK\r\n")) << rsp;
  ASSERT_TRUE(rsp.ends_with("Content-Length: 4\r\nConnection: close\r\n\r\npong")) << rsp;

  ASSERT_NE(get(addr(), "/").find("/connections\n/echo\n/ping\n/server\n"), std::string::npos);
  ASSERT_NE(get(addr(), "/server").find("Listening"), std::string::npos);
  ASSERT_NE(get(addr(), "/connections").find("accepted valid"), std::string::npos);

  ASSERT_TRUE(get(addr(), "/nope").starts_with("HTTP/1.1 404 "));
  ASSERT_TRUE(rawRequest(addr(), {"POST /ping HTTP/1.1\r\nContent-Length: 0\r\n\r\n"}).starts_with("HTTP/1.1 405 "));
  ASSERT_TRUE(rawRequest(addr(), {"hello\r\n\r\n"}).starts_with("HTTP/1.1 400 "));
  // exactly fills the 32KB read buffer, so the server has nothing unread when it closes (no RST).
  ASSERT_TRUE(rawRequest(addr(), {std::string(kMessageReadBufferSize, 'G')}).starts_with("HTTP/1.1 431 "));
  // half packet: the read task must suspend and resume, not hang.
  ASSERT_TRUE(rawRequest(addr(), {"GET /pi", "ng HTTP/1.1\r\n\r\n"}).starts_with("HTTP/1.1 200 "));

  checkEcho();
}

TEST_P(TestHttp, Head) {
  serverConfig_.groups(0).processor().set_enable_http(true);
  ASSERT_TRUE(server_.start());

  auto getRsp = get(addr(), "/ping");
  auto headRsp = rawRequest(addr(), {"HEAD /ping HTTP/1.1\r\n\r\n"});
  ASSERT_EQ(headRsp, getRsp.substr(0, getRsp.size() - 4));
  ASSERT_NE(headRsp.find("Content-Length: 4\r\n"), std::string::npos);
  auto notFound = rawRequest(addr(), {"HEAD /nope HTTP/1.1\r\n\r\n"});
  ASSERT_TRUE(notFound.starts_with("HTTP/1.1 404 ")) << notFound;
  ASSERT_TRUE(notFound.ends_with("\r\n\r\n")) << notFound;
}

TEST_P(TestHttp, Post) {
  serverConfig_.groups(0).processor().set_enable_http(true);
  ASSERT_TRUE(server_.start());

  ASSERT_TRUE(post(addr(), "hello").ends_with("\r\n\r\n5 hello")) << post(addr(), "hello");
  ASSERT_TRUE(post(addr(), "").ends_with("\r\n\r\n0 "));
  // body in two parts, and a 1MB body that needs the read buffer to grow.
  auto rsp = rawRequest(addr(), {"POST /echo HTTP/1.1\r\nContent-Length: 10\r\n\r\nabcde", "fghij"});
  ASSERT_TRUE(rsp.ends_with("\r\n\r\n10 abcdefghij")) << rsp;
  rsp = post(addr(), std::string(1 << 20, 'x'));
  ASSERT_TRUE(rsp.ends_with("\r\n\r\n1048576 xxxxxxxxxxxxxxxx")) << rsp;

  ASSERT_TRUE(rawRequest(addr(), {"POST /echo HTTP/1.1\r\n\r\n"}).starts_with("HTTP/1.1 411 "));
  ASSERT_TRUE(rawRequest(addr(), {"POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"})
                  .starts_with("HTTP/1.1 501 "));
  auto notAllowed = post(addr(), "x", "/ping");
  ASSERT_TRUE(notAllowed.starts_with("HTTP/1.1 405 ")) << notAllowed;
  ASSERT_NE(notAllowed.find("Allow: GET, HEAD\r\n"), std::string::npos);
  for (auto method : {"PUT", "DELETE"}) {
    auto r = rawRequest(addr(), {fmt::format("{} /echo HTTP/1.1\r\n\r\n", method)});
    ASSERT_TRUE(r.starts_with("HTTP/1.1 405 ")) << r;
    ASSERT_NE(r.find("Allow: GET, HEAD, POST\r\n"), std::string::npos);
  }
  // rejected before the body is read, no "100 Continue".
  ASSERT_TRUE(rawRequest(addr(), {"POST /ping HTTP/1.1\r\nContent-Length: 10\r\nExpect: 100-continue\r\n\r\n"})
                  .starts_with("HTTP/1.1 405 "));
  ASSERT_TRUE(rawRequest(addr(), {"POST /nope HTTP/1.1\r\nContent-Length: 10\r\nExpect: 100-continue\r\n\r\n"})
                  .starts_with("HTTP/1.1 404 "));
  ASSERT_TRUE(
      rawRequest(addr(), {"POST /echo HTTP/1.1\r\nContent-Length: 17000000\r\nExpect: 100-continue\r\n\r\n"})
          .starts_with("HTTP/1.1 413 "));
  ASSERT_TRUE(rawRequest(addr(), {"POST /echo HTTP/1.1\r\nContent-Length: 1\r\nExpect: foo\r\n\r\n"})
                  .starts_with("HTTP/1.1 417 "));

  // 100 Continue, then the body, once.
  int fd = connectTo(addr());
  sendAll(fd, "POST /echo HTTP/1.1\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n");
  std::string_view kContinue = "HTTP/1.1 100 Continue\r\n\r\n";
  std::string got(kContinue.size(), 0);
  ASSERT_EQ(::recv(fd, got.data(), got.size(), MSG_WAITALL), ssize_t(got.size()));
  ASSERT_EQ(got, kContinue);
  sendAll(fd, "he");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  sendAll(fd, "llo");
  rsp = readToEnd(fd);
  ASSERT_TRUE(rsp.starts_with("HTTP/1.1 200 ")) << rsp;
  ASSERT_TRUE(rsp.ends_with("5 hello")) << rsp;

  checkEcho();
}

TEST_P(TestHttp, BodyTooLarge) {
  serverConfig_.groups(0).processor().set_enable_http(true);
  serverConfig_.groups(0).processor().set_http_max_body_size(1024);
  ASSERT_TRUE(server_.start());
  // the whole body is already in the buffer, still rejected.
  ASSERT_TRUE(post(addr(), std::string(2048, 'x')).starts_with("HTTP/1.1 413 "));
  ASSERT_TRUE(post(addr(), std::string(1024, 'x')).starts_with("HTTP/1.1 200 "));
}

TEST_P(TestHttp, Disabled) {
  ASSERT_TRUE(server_.start());
  ASSERT_EQ(get(addr(), "/ping"), "");  // parsed as a too large serde message, connection dropped.
  checkEcho();
}

INSTANTIATE_TEST_SUITE_P(EventThread, TestHttp, testing::Bool());

}  // namespace
}  // namespace hf3fs::net::test
