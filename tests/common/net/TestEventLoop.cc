#include <fcntl.h>
#include <folly/Random.h>
#include <gtest/gtest.h>

#include "common/net/EventLoop.h"
#include "common/net/Network.h"
#include "common/utils/FdWrapper.h"

namespace hf3fs::net::test {
namespace {

TEST(TestEventLoop, Normal) {
  auto eventLoop = EventLoop::create();
  ASSERT_TRUE(eventLoop->start());
  eventLoop->stopAndJoin();
}

TEST(TestEventLoop, AddObject) {
  auto eventLoop = EventLoop::create();
  ASSERT_TRUE(eventLoop->start());

  class PipeReader : public EventLoop::EventHandler {
   public:
    PipeReader(int fd)
        : fd_(fd) {}

    int fd() const final { return fd_; }
    void handleEvents(uint32_t epollEvents) final {
      ASSERT_TRUE(epollEvents & EPOLLIN);
      uint32_t val;
      ASSERT_EQ(::read(fd_, &val, sizeof(val)), sizeof(val));
      val_ = val;
    }

    auto &val() const { return val_; }

   private:
    FdWrapper fd_;
    std::atomic<uint32_t> val_{0};
  };

  int p[2];
  ASSERT_EQ(::pipe(p), 0);
  auto pipeReader = std::make_shared<PipeReader>(p[0]);
  ASSERT_TRUE(eventLoop->add(pipeReader, EPOLLIN | EPOLLET));
  ASSERT_EQ(pipeReader->val().load(), 0);

  uint32_t val = rand();
  ASSERT_EQ(::write(p[1], &val, sizeof(val)), sizeof(val));
  while (pipeReader->val().load() == 0) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_EQ(pipeReader->val().load(), val);

  ASSERT_TRUE(eventLoop->remove(pipeReader.get()));
  std::this_thread::sleep_for(10ms);
}

// a fake CQ: work only raises an "interrupt" (pipe write) while armed, like ibv_req_notify_cq.
class FakeCQ : public EventLoop::EventHandler {
 public:
  FakeCQ() {
    int p[2];
    EXPECT_EQ(::pipe2(p, O_NONBLOCK), 0);
    rd_ = p[0];
    wr_ = p[1];
  }

  int fd() const final { return rd_; }
  void handleEvents(uint32_t) final {
    char buf[64];
    while (::read(rd_, buf, sizeof(buf)) > 0) {
    }
    ++wakeups;
    consume();
  }
  bool busyPoll(bool arm) final {
    if (arm) armed_ = true;
    return consume();
  }

  void produce() {
    ++pending_;
    if (armed_.exchange(false)) {
      char c = 0;
      EXPECT_EQ(::write(wr_, &c, 1), 1);
    }
  }

  std::atomic<uint64_t> consumed{0};
  std::atomic<uint64_t> wakeups{0};

 private:
  bool consume() {
    auto n = pending_.exchange(0);
    consumed += n;
    return n != 0;
  }

  FdWrapper rd_;
  FdWrapper wr_;
  std::atomic<uint64_t> pending_{0};
  std::atomic<bool> armed_{false};
};

static void runBusyPoll(std::chrono::microseconds idle) {
  auto eventLoop = EventLoop::create();
  ASSERT_TRUE(eventLoop->start("BusyPoll", {.enable = true, .idle = idle}));
  auto cq = std::make_shared<FakeCQ>();
  ASSERT_TRUE(eventLoop->add(cq, EPOLLIN | EPOLLET));

  // gaps around the idle threshold so the hybrid loop goes to sleep and races with arming.
  constexpr uint64_t kN = 300;
  for (uint64_t i = 0; i < kN; ++i) {
    cq->produce();
    std::this_thread::sleep_for(std::chrono::microseconds(folly::Random::rand32(400)));
  }
  for (int i = 0; i < 100 && cq->consumed < kN; ++i) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_EQ(cq->consumed.load(), kN);
  if (idle.count() == 0) {
    ASSERT_EQ(cq->wakeups.load(), 0);  // never sleeps, never needs an interrupt.
  } else {
    ASSERT_GT(cq->wakeups.load(), 0);
  }
  ASSERT_TRUE(eventLoop->remove(cq.get()));
  eventLoop->stopAndJoin();
}

TEST(TestEventLoop, BusyPoll) { runBusyPoll(std::chrono::microseconds(0)); }

TEST(TestEventLoop, HybridPoll) { runBusyPoll(std::chrono::microseconds(100)); }

}  // namespace
}  // namespace hf3fs::net::test
