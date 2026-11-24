// Minimal base-rpc usage: a TCP echo server and a synchronous client in one process.
#include <folly/init/Init.h>
#include <iostream>

#include "common/net/Server.h"
#include "common/net/sync/Client.h"
#include "common/serde/ClientContext.h"
#include "common/serde/Service.h"

using namespace hf3fs;

struct EchoReq {
  SERDE_STRUCT_FIELD(val, std::string{});
};
struct EchoRsp {
  SERDE_STRUCT_FIELD(val, std::string{});
};

// service name "Echo", service id 86; method echo has id 1.
SERDE_SERVICE(Echo, 86) { SERDE_SERVICE_METHOD(echo, 1, EchoReq, EchoRsp); };

class EchoServiceImpl : public serde::ServiceWrapper<EchoServiceImpl, Echo> {
 public:
  CoTryTask<EchoRsp> echo(serde::CallContext &, const EchoReq &req) {
    EchoRsp rsp;
    rsp.val = req.val;
    co_return rsp;
  }
};

int main(int argc, char *argv[]) {
  folly::init(&argc, &argv);

  net::Server::Config serverConfig;
  serverConfig.groups(0).set_network_type(net::Address::TCP);
  net::Server server{serverConfig};
  server.addSerdeService(std::make_unique<EchoServiceImpl>());
  if (auto r = server.setup(); r.hasError()) {
    std::cerr << "setup failed: " << r.error().describe() << std::endl;
    return 1;
  }
  if (auto r = server.start(); r.hasError()) {
    std::cerr << "start failed: " << r.error().describe() << std::endl;
    return 1;
  }

  net::sync::Client::Config clientConfig;
  net::sync::Client client{clientConfig};
  auto ctx = client.serdeCtx(server.groups().front()->addressList().front());

  EchoReq req;
  req.val = "hello base-rpc";
  auto result = Echo<>{}.echoSync(ctx, req);
  if (result.hasError()) {
    std::cerr << "call failed: " << result.error().describe() << std::endl;
    return 1;
  }
  std::cout << "echo: " << result->val << std::endl;
  server.stopAndJoin();
  return result->val == req.val ? 0 : 1;
}
