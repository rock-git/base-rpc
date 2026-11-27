// HTTP/1.1 GET/HEAD/POST server on the RPC TCP port (docs/http-sniff.md, docs/http-post-head.md), for checking with curl.
//   server: http_server --port=8000
//   curl -i http://127.0.0.1:8000/ping
//   curl -I http://127.0.0.1:8000/ping
//   curl -d hello http://127.0.0.1:8000/echo
#include <atomic>
#include <csignal>
#include <folly/init/Init.h>
#include <iostream>
#include <pthread.h>

#include "common/net/Server.h"

using namespace hf3fs;

DEFINE_uint32(port, 8000, "TCP port to listen on");

int main(int argc, char *argv[]) {
  folly::init(&argc, &argv);
  // block before any thread starts so that sigwait() below gets the signal.
  sigset_t sigs;
  sigemptyset(&sigs);
  sigaddset(&sigs, SIGINT);
  sigaddset(&sigs, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

  net::Server::Config config;
  config.groups(0).set_network_type(net::Address::TCP);
  config.groups(0).listener().set_listen_port(FLAGS_port);
  config.groups(0).processor().set_enable_http(true);
  net::Server server{config};

  static std::atomic<uint64_t> count{0};
  auto &group = *server.groups().front();
  group.addHttpRoute("/ping", [](const net::http::Request &) { return net::Processor::HttpResponse{.body = "pong\n"}; });
  group.addHttpRoute("/json", [](const net::http::Request &) {
    return net::Processor::HttpResponse{.contentType = "application/json", .body = "{\"ok\":true}\n"};
  });
  group.addHttpRoute("/count", [](const net::http::Request &) { return net::Processor::HttpResponse{.body = std::to_string(++count) + "\n"}; });

  // small response on purpose: replies must fit in the socket send buffer.
  group.addHttpRoute(
      "/echo",
      [](const net::http::Request &req) {
        return net::Processor::HttpResponse{.body = std::to_string(req.body.size()) + " " +
                                                    std::string(req.body.substr(0, 64)) + "\n"};
      },
      true);

  if (auto r = server.setup(); r.hasError()) {
    std::cerr << "setup failed: " << r.error().describe() << std::endl;
    return 1;
  }
  if (auto r = server.start(); r.hasError()) {
    std::cerr << "start failed: " << r.error().describe() << std::endl;
    return 1;
  }
  std::cout << "listening on port " << FLAGS_port << ", e.g. curl -i http://127.0.0.1:" << FLAGS_port << "/ping"
            << std::endl;

  int sig = 0;
  sigwait(&sigs, &sig);
  std::cout << "got signal " << sig << ", stopping" << std::endl;
  server.stopAndJoin();
  return 0;
}
