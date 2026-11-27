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

// send `parts` over a blocking socket (pausing between them) and read until the server closes.
std::string rawRequest(Address addr, std::vector<std::string> parts) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_GE(fd, 0);
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(addr.port);
  sa.sin_addr.s_addr = addr.ip;
  EXPECT_EQ(::connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)), 0);
  for (auto &part : parts) {
    EXPECT_EQ(::send(fd, part.data(), part.size(), MSG_NOSIGNAL), ssize_t(part.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::string out;
  char buf[4096];
  ssize_t n;
  while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
    out.append(buf, n);
  }
  ::close(fd);
  return out;
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
  ASSERT_EQ(http::formatResponse(http::Response::error(404, "nope")),
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 5\r\n"
            "Connection: close\r\n\r\nnope\n");
}

class TestHttp : public testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    server_.addSerdeService(std::make_unique<EchoImpl>());
    server_.groups().front()->addHttpRoute("/ping", [] { return Processor::HttpResponse{.body = "pong"}; });
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

  ASSERT_NE(get(addr(), "/").find("/connections\n/ping\n/server\n"), std::string::npos);
  ASSERT_NE(get(addr(), "/server").find("Listening"), std::string::npos);
  ASSERT_NE(get(addr(), "/connections").find("accepted valid"), std::string::npos);

  ASSERT_TRUE(get(addr(), "/nope").starts_with("HTTP/1.1 404 "));
  ASSERT_TRUE(rawRequest(addr(), {"POST /ping HTTP/1.1\r\n\r\n"}).starts_with("HTTP/1.1 405 "));
  ASSERT_TRUE(rawRequest(addr(), {"hello\r\n\r\n"}).starts_with("HTTP/1.1 400 "));
  // exactly fills the 32KB read buffer, so the server has nothing unread when it closes (no RST).
  ASSERT_TRUE(rawRequest(addr(), {std::string(kMessageReadBufferSize, 'G')}).starts_with("HTTP/1.1 431 "));
  // half packet: the read task must suspend and resume, not hang.
  ASSERT_TRUE(rawRequest(addr(), {"GET /pi", "ng HTTP/1.1\r\n\r\n"}).starts_with("HTTP/1.1 200 "));

  checkEcho();
}

TEST_P(TestHttp, Disabled) {
  ASSERT_TRUE(server_.start());
  ASSERT_EQ(get(addr(), "/ping"), "");  // parsed as a too large serde message, connection dropped.
  checkEcho();
}

INSTANTIATE_TEST_SUITE_P(EventThread, TestHttp, testing::Bool());

}  // namespace
}  // namespace hf3fs::net::test
