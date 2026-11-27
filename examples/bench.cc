// RPC benchmark based on echo.cc. Closed-loop async client: `--concurrency` coroutines each keep one request in flight.
//   server: bench --mode=server --proto=tcp|rdma|local --addr=0.0.0.0:8000
//   client: bench --mode=client --proto=tcp|rdma|local --addr=<server ip>:8000 --concurrency=64 --duration=10 --total=0
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/init/Init.h>
#include <iostream>
#include <thread>

#include "common/net/Client.h"
#include "common/net/IfAddrs.h"
#include "common/net/Server.h"
#include "common/net/ib/IBDevice.h"
#include "common/serde/ClientContext.h"
#include "common/serde/Service.h"
#include "common/utils/DynamicCoroutinesPool.h"

DEFINE_string(mode, "", "server|client");
DEFINE_string(proto, "tcp", "tcp|rdma|local (local: server listens on loopback \"lo\" instead of physical NICs; client is plain TCP)");
DEFINE_string(addr, "127.0.0.1:8000", "client: server ip:port; server: bind address, 8000 = :8000 = 0.0.0.0:8000 = all NICs; a specific ip binds all addresses of that NIC)");
DEFINE_uint32(concurrency, 64, "client: number of in-flight requests");
DEFINE_uint32(duration, 10, "client: run time in seconds, 0 = unlimited");
DEFINE_uint64(total, 0, "client: total requests, 0 = unlimited (stops at whichever of duration/total comes first)");
DEFINE_uint32(payload, 64, "client: payload bytes per request");
DEFINE_bool(rw_in_event_thread, false, "read/write sockets directly in the event-loop thread (fewer thread hops)");
DEFINE_uint32(proc_threads, 2, "processor threads (server: request handling; client: response dispatch)");
DEFINE_string(proc_strategy, "SHARED_QUEUE", "proc pool strategy: SHARED_QUEUE|SHARED_NOTHING|WORK_STEALING|ROUND_ROBIN|GROUP_WAITING_4|GROUP_WAITING_8");
DEFINE_uint32(io_threads, 2, "io threads");
DEFINE_uint32(bg_threads, 2, "client: threads that run the benchmark coroutines");
DEFINE_uint32(coro_pool_threads, 0, "server: >0 handles requests in a DynamicCoroutinesPool with this many threads");
DEFINE_string(rdma_poll_mode, "EVENT", "rdma completion polling: EVENT (epoll) | BUSY (spin) | HYBRID (spin, then sleep when idle)");
DEFINE_uint32(busy_poll_idle_us, 100, "HYBRID: spin this long without work before sleeping in epoll");
DEFINE_uint32(rdma_pollers, 1, "BUSY/HYBRID: number of poller threads");
DEFINE_uint64(unpack_inline_max_size, 0, "unpack received batches up to this size in the io thread (0 = hop to proc pool)");
DEFINE_bool(trace, true, "client: collect per-stage latency from the rpc timestamp (adds a small header to each packet)");

using namespace hf3fs;
using Clock = std::chrono::steady_clock;

struct EchoReq {
  SERDE_STRUCT_FIELD(val, std::string{});
};
struct EchoRsp {
  SERDE_STRUCT_FIELD(val, std::string{});
};
SERDE_SERVICE(Echo, 86) { SERDE_SERVICE_METHOD(echo, 1, EchoReq, EchoRsp); };

class EchoServiceImpl : public serde::ServiceWrapper<EchoServiceImpl, Echo> {
 public:
  CoTryTask<EchoRsp> echo(serde::CallContext &, const EchoReq &req) {
    EchoRsp rsp;
    rsp.val = req.val;
    co_return rsp;
  }
};

static std::atomic<bool> gStop{false};
static CPUExecutorGroup::ExecutorStrategy kProcStrategy;
static net::RDMAPollMode kPollMode;

static void setPolling(net::IOWorker::Config &io, net::Processor::Config &proc) {
  io.set_rdma_poll_mode(kPollMode);
  io.set_busy_poll_idle_us(FLAGS_busy_poll_idle_us);
  io.set_num_rdma_pollers(FLAGS_rdma_pollers);
  proc.set_unpack_inline_max_size(FLAGS_unpack_inline_max_size);
}

static int runServer(net::Address::Type type, net::Address bind) {
  net::Server::Config config;
  config.groups(0).set_network_type(type);
  config.groups(0).listener().set_listen_port(bind.port);
  // The listener binds every NIC address by default and can only be narrowed by NIC name, so map the IP to its NIC.
  if (bind.ip != 0) {
    std::set<std::string> nics;
    if (auto all = net::IfAddrs::load(); all.hasValue()) {
      for (auto &[name, a] : *all) {
        if (a.ip.toLong() == bind.ip) nics.insert(name);
      }
    }
    if (nics.empty()) {
      std::cerr << "no NIC has address " << FLAGS_addr << std::endl;
      return 1;
    }
    config.groups(0).listener().set_filter_list(nics);
  }
  config.groups(0).io_worker().set_read_write_tcp_in_event_thread(FLAGS_rw_in_event_thread);
  config.groups(0).io_worker().set_read_write_rdma_in_event_thread(FLAGS_rw_in_event_thread);
  setPolling(config.groups(0).io_worker(), config.groups(0).processor());
  config.thread_pool().set_num_proc_threads(FLAGS_proc_threads);
  config.thread_pool().set_proc_thread_pool_stratetry(kProcStrategy);
  config.thread_pool().set_num_io_threads(FLAGS_io_threads);
  net::Server server{config};
  server.addSerdeService(std::make_unique<EchoServiceImpl>());
  if (auto r = server.setup(); r.hasError()) {
    std::cerr << "setup failed: " << r.error().describe() << std::endl;
    return 1;
  }
  DynamicCoroutinesPool::Config poolConfig;
  poolConfig.set_threads_num(FLAGS_coro_pool_threads);
  DynamicCoroutinesPool pool{poolConfig, "BenchPool"};
  if (FLAGS_coro_pool_threads) {
    pool.start();
    server.groups().front()->setCoroutinesPoolGetter([&](auto &) -> DynamicCoroutinesPool & { return pool; });
  }
  if (auto r = server.start(); r.hasError()) {
    std::cerr << "start failed: " << r.error().describe() << std::endl;
    return 1;
  }
  for (auto &a : server.groups().front()->addressList()) std::cout << "listening on " << a.toString() << std::endl;
  while (!gStop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  server.stopAndJoin();
  return 0;
}

// Stages of one RPC, in order. Timestamps are UTC microseconds. "network" = request + response wire time, computed as
// (clientReceived - clientSerialized) - server time, so it needs no cross-host clock sync (unlike each direction alone).
static constexpr const char *kStages[] = {"client_pre_send", "network", "server_queue",
                                          "server_process", "server_serialize", "client_post_recv", "total(rpc)"};
constexpr size_t kNumStages = std::size(kStages);

static std::array<int64_t, kNumStages> stageUs(const serde::Timestamp &t) {
  return {t.clientSerialized - t.clientCalled,
          (t.clientReceived - t.clientSerialized) - (t.serverSerialized - t.serverReceived),
          t.serverWaked - t.serverReceived,
          t.serverProcessed - t.serverWaked,
          t.serverSerialized - t.serverProcessed,
          t.clientWaked - t.clientReceived,
          t.clientWaked - t.clientCalled};
}

struct Worker {
  std::vector<uint32_t> latencyUs;
  std::array<std::vector<uint32_t>, kNumStages> stages;
  uint64_t failed = 0;
};

static int runClient(net::Address addr) {
  net::Client::Config config;
  config.io_worker().set_read_write_tcp_in_event_thread(FLAGS_rw_in_event_thread);
  config.io_worker().set_read_write_rdma_in_event_thread(FLAGS_rw_in_event_thread);
  setPolling(config.io_worker(), config.processor());
  config.thread_pool().set_num_proc_threads(FLAGS_proc_threads);
  config.thread_pool().set_proc_thread_pool_stratetry(kProcStrategy);
  config.thread_pool().set_num_io_threads(FLAGS_io_threads);
  config.thread_pool().set_num_bg_threads(FLAGS_bg_threads);
  net::Client client{config};
  if (auto r = client.start(); r.hasError()) {
    std::cerr << "client start failed: " << r.error().describe() << std::endl;
    return 1;
  }
  auto ctx = client.serdeCtx(addr);
  EchoReq req;
  req.val.assign(FLAGS_payload, 'x');

  std::atomic<uint64_t> issued{0};
  const uint64_t total = FLAGS_total;
  const auto begin = Clock::now();
  const auto deadline = begin + std::chrono::seconds(FLAGS_duration);
  std::atomic<bool> firstError{true};

  std::vector<Worker> workers(FLAGS_concurrency);
  auto loop = [&](Worker &w) -> CoTask<void> {
    while (!gStop) {
      if (FLAGS_duration && Clock::now() >= deadline) break;
      if (total && issued.fetch_add(1) >= total) break;
      auto t0 = Clock::now();
      serde::Timestamp ts;
      auto result = co_await Echo<>{}.echo(ctx, req, nullptr, FLAGS_trace ? &ts : nullptr);
      auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
      if (result.hasError() || result->val.size() != req.val.size()) {
        ++w.failed;
        if (firstError.exchange(false)) {
          std::cerr << "request failed: " << (result.hasError() ? result.error().describe() : "bad response") << "\n";
        }
        continue;
      }
      w.latencyUs.push_back(static_cast<uint32_t>(us));
      if (FLAGS_trace) {
        auto v = stageUs(ts);
        for (size_t i = 0; i < kNumStages; ++i) w.stages[i].push_back(static_cast<uint32_t>(std::max<int64_t>(v[i], 0)));
      }
    }
  };

  std::vector<folly::SemiFuture<folly::Unit>> futures;
  for (auto &w : workers) {
    futures.push_back(loop(w).scheduleOn(&client.tpg().bgThreadPool().randomPick()).start().deferValue([](auto &&) {
      return folly::unit;
    }));
  }
  folly::coro::blockingWait(folly::coro::collectAllRange(
      futures | std::views::transform([](auto &f) -> folly::coro::Task<void> { co_await std::move(f); })));
  const double elapsed = std::chrono::duration<double>(Clock::now() - begin).count();

  std::vector<uint32_t> all;
  std::array<std::vector<uint32_t>, kNumStages> stages;
  uint64_t failed = 0;
  for (auto &w : workers) {
    all.insert(all.end(), w.latencyUs.begin(), w.latencyUs.end());
    for (size_t i = 0; i < kNumStages; ++i) stages[i].insert(stages[i].end(), w.stages[i].begin(), w.stages[i].end());
    failed += w.failed;
  }
  std::sort(all.begin(), all.end());
  client.stopAndJoin();
  if (all.empty()) {
    std::cerr << "no successful request, failed=" << failed << std::endl;
    return 1;
  }
  auto pctOf = [](const std::vector<uint32_t> &v, double p) {
    return v[std::min(v.size() - 1, static_cast<size_t>(p / 100 * v.size()))];
  };
  auto pct = [&](double p) { return pctOf(all, p); };
  double sum = 0;
  for (auto v : all) sum += v;
  printf("proto=%s concurrency=%u payload=%u elapsed=%.2fs\n", FLAGS_proto.c_str(), FLAGS_concurrency, FLAGS_payload, elapsed);
  printf("requests=%zu failed=%lu\n", all.size(), failed);
  printf("qps=%.0f\n", all.size() / elapsed);
  printf("latency(us): avg=%.1f p50=%u p95=%u p99=%u p99.9=%u max=%u\n",
         sum / all.size(), pct(50), pct(95), pct(99), pct(99.9), all.back());
  if (FLAGS_trace) {
    printf("stage latency(us):          avg     p50     p95     p99   p99.9     max\n");
    for (size_t i = 0; i < kNumStages; ++i) {
      auto &v = stages[i];
      std::sort(v.begin(), v.end());
      double s = 0;
      for (auto x : v) s += x;
      printf("  %-18s %9.1f %7u %7u %7u %7u %7u\n", kStages[i], s / v.size(), pctOf(v, 50), pctOf(v, 95),
             pctOf(v, 99), pctOf(v, 99.9), v.back());
    }
  }
  return 0;
}

int main(int argc, char *argv[]) {
  folly::init(&argc, &argv);
  if ((FLAGS_mode != "server" && FLAGS_mode != "client") || (FLAGS_proto != "tcp" && FLAGS_proto != "rdma" && FLAGS_proto != "local")) {
    std::cerr << "usage: --mode=server|client --proto=tcp|rdma|local [--addr=ip:port --concurrency=N --duration=S --total=N "
                 "--payload=B]\n";
    return 1;
  }
  auto strategy = magic_enum::enum_cast<CPUExecutorGroup::ExecutorStrategy>(FLAGS_proc_strategy);
  if (!strategy) {
    std::cerr << "invalid --proc_strategy: " << FLAGS_proc_strategy << std::endl;
    return 1;
  }
  kProcStrategy = *strategy;
  auto pollMode = magic_enum::enum_cast<net::RDMAPollMode>(FLAGS_rdma_poll_mode);
  if (!pollMode) {
    std::cerr << "invalid --rdma_poll_mode: " << FLAGS_rdma_poll_mode << std::endl;
    return 1;
  }
  kPollMode = *pollMode;
  const auto type = FLAGS_proto == "rdma" ? net::Address::RDMA : net::Address::TCP;
  // the server's LOCAL network type only changes which NIC it listens on; clients connect over TCP.
  const auto serverType = FLAGS_proto == "local" ? net::Address::LOCAL : type;
  // server: "8000" and ":8000" mean "0.0.0.0:8000".
  if (FLAGS_mode == "server" && (FLAGS_addr.starts_with(':') || FLAGS_addr.find_first_not_of("0123456789") == FLAGS_addr.npos)) {
    FLAGS_addr = std::string("0.0.0.0") + (FLAGS_addr.starts_with(':') ? "" : ":") + FLAGS_addr;
  }
  auto addr = net::Address::fromString(FLAGS_addr, type);
  if (!addr || (FLAGS_mode == "client" && addr.ip == 0)) {
    std::cerr << "invalid --addr (client needs ip:port): " << FLAGS_addr << std::endl;
    return 1;
  }
  if (FLAGS_mode == "client" && (FLAGS_concurrency == 0 || (FLAGS_duration == 0 && FLAGS_total == 0))) {
    std::cerr << "need --concurrency>0 and at least one of --duration/--total\n";
    return 1;
  }
  std::signal(SIGINT, [](int) { gStop = true; });
  std::signal(SIGTERM, [](int) { gStop = true; });

  if (type == net::Address::RDMA) {
    auto r = net::IBManager::start(net::IBConfig{});
    if (r.hasError() || net::IBDevice::all().empty()) {
      std::cerr << "RDMA init failed: " << (r.hasError() ? r.error().describe() : "no RDMA device") << std::endl;
      return 1;
    }
  }
  int rc = FLAGS_mode == "server" ? runServer(serverType, addr) : runClient(addr);
  if (type == net::Address::RDMA) net::IBManager::stop();
  return rc;
}
