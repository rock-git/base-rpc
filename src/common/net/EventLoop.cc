#include "common/net/EventLoop.h"

#include <folly/Random.h>
#include <folly/logging/xlog.h>
#include <folly/portability/Asm.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

#include "common/net/Network.h"
#include "common/utils/Size.h"

namespace hf3fs::net {

Result<Void> EventLoop::start(const std::string &threadName, BusyPollOptions busy) {
  busy_ = busy;
  // 1. init epoll fd.
  epfd_ = ::epoll_create(16_KB);
  if (UNLIKELY(!epfd_.valid())) {
    XLOGF(ERR, "create epoll failed");
    return makeError(RPCCode::kEpollInitError, "create epoll failed");
  }

  // 2. init event fd for notify.
  eventfd_ = ::eventfd(0, EFD_NONBLOCK);
  if (UNLIKELY(!eventfd_.valid())) {
    XLOGF(ERR, "create eventfd failed");
    return makeError(RPCCode::kEpollInitError, "create eventfd failed");
  }

  // 3. add event fd into epoll.
  struct epoll_event evt = {EPOLLIN | EPOLLET, {nullptr}};
  int ret = ::epoll_ctl(epfd_, EPOLL_CTL_ADD, eventfd_, &evt);
  if (UNLIKELY(ret == -1)) {
    auto msg = fmt::format("add eventfd into epoll failed, epoll {}, eventfd {}, errno {}", epfd_, eventfd_, errno);
    XLOG(ERR, msg);
    return makeError(RPCCode::kEpollAddError, std::move(msg));
  }

  // 4. start loop in background thread.
  thread_ = std::jthread(busy_.enable ? &EventLoop::busyLoop : &EventLoop::loop, this);
  folly::setThreadName(thread_.get_id(), threadName);
  if (busy_.cpu >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(busy_.cpu, &set);
    int ret = ::pthread_setaffinity_np(thread_.native_handle(), sizeof(set), &set);
    XLOGF_IF(WARN, ret != 0, "EventLoop {} failed to pin to cpu {}, error {}", threadName, busy_.cpu, ret);
  }
  return Void{};
}

Result<Void> EventLoop::wakeUp() {
  uint64_t val = 1;
  int ret = ::write(eventfd_, &val, sizeof(val));
  if (ret == -1) {
    auto msg = fmt::format("wake up epoll loop failed, eventfd {}, errno {}", eventfd_, errno);
    XLOG(ERR, msg);
    return makeError(RPCCode::kEpollWakeUpError, std::move(msg));
  }
  return Void{};
}

void EventLoop::stopAndJoin() {
  if (thread_.joinable()) {
    stop_ = true;
    wakeUp();
    thread_.join();
  }
}

Result<Void> EventLoop::add(const std::shared_ptr<EventHandler> &handler, uint32_t interestEvents) {
  HandlerWrapper *wrapper = nullptr;
  {
    auto lock = std::unique_lock(mutex_);
    wrapperList_.emplace_front(HandlerWrapper{handler});
    handler->it_ = wrapperList_.begin();
    handler->eventLoop_ = weak_from_this();
    wrapper = &wrapperList_.front();
    ++version_;
  }

  struct epoll_event event;
  event.events = interestEvents;
  event.data.ptr = wrapper;
  int ret = ::epoll_ctl(epfd_, EPOLL_CTL_ADD, handler->fd(), &event);
  if (ret == 0) {
    return Void{};
  }

  // remove from list if fail to add.
  {
    auto lock = std::unique_lock(mutex_);
    wrapperList_.erase(handler->it_);
  }
  handler->it_ = std::list<HandlerWrapper>::iterator{};
  handler->eventLoop_.reset();
  auto msg = fmt::format("add fd into epoll failed, epoll {}, fd {}, errno {}", epfd_, handler->fd(), errno);
  XLOG(ERR, msg);
  return makeError(RPCCode::kEpollAddError, std::move(msg));
}

Result<Void> EventLoop::remove(EventHandler *handler) {
  if (handler->it_ == std::list<HandlerWrapper>::iterator{}) {
    XLOGF(DBG, "try to remove a invalid event handler, epoll {}, fd {}", epfd_, handler->fd());
    return Void{};
  }

  int ret = ::epoll_ctl(epfd_, EPOLL_CTL_DEL, handler->fd(), nullptr);
  if (ret == -1) {
    auto msg = fmt::format("remove fd from epoll failed, epoll {}, fd {}, errno {}", epfd_, handler->fd(), errno);
    XLOG(ERR, msg);
    return makeError(RPCCode::kEpollDelError, std::move(msg));
  }

  deleteQueue_.enqueue(handler->it_);
  handler->it_ = std::list<HandlerWrapper>::iterator{};

  // wake up event loop if size of delete queue is greater than threshold.
  if (deleteQueue_.size() >= kDeleteQueueWakeUpLoopThreshold) {
    wakeUp();
  }
  return Void{};
}

void EventLoop::loop() {
  XLOGF(INFO, "EventLoop::loop() started.");

  while (true) {
    // 1. wait events.
    constexpr int kMaxEvents = 64;
    struct epoll_event events[kMaxEvents];
    int n = ::epoll_wait(epfd_, events, kMaxEvents, -1);
    if (n == -1) {
      XLOGF(ERR, "epoll_wait failed, errno {}, retry", errno);
      continue;
    }
    if (stop_) {
      break;
    }

    dispatch(events, n);
    processDeletes();
  }

  XLOGF(INFO, "EventLoop::loop() stopped.");
}

void EventLoop::busyLoop() {
  XLOGF(INFO, "EventLoop::busyLoop() started, idle {}us.", busy_.idle.count());

  // wrappers are only erased in this thread (processDeletes), so the raw pointers stay valid until the next refresh.
  std::vector<HandlerWrapper *> handlers;
  uint64_t seenVersion = ~0ull;
  auto pollAll = [&](bool arm) {
    bool work = false;
    for (auto wrapper : handlers) {
      if (auto handler = wrapper->handler.lock()) {
        work |= handler->busyPoll(arm);
      }
    }
    return work;
  };

  auto lastWork = std::chrono::steady_clock::now();
  while (!stop_) {
    if (auto version = version_.load(std::memory_order_acquire); version != seenVersion) {
      auto lock = std::unique_lock(mutex_);
      seenVersion = version;
      handlers.clear();
      for (auto &wrapper : wrapperList_) {
        handlers.push_back(&wrapper);
      }
    }

    auto now = std::chrono::steady_clock::now();
    if (pollAll(false)) {
      lastWork = now;
    } else if (busy_.idle.count() == 0 || now - lastWork < busy_.idle) {
      folly::asm_volatile_pause();
    } else if (pollAll(true)) {
      // work arrived while arming, keep spinning.
      // shortcut: handlers armed here stay armed while spinning, costing at most one spurious interrupt per sleep.
      lastWork = now;
    } else {
      constexpr int kMaxEvents = 64;
      struct epoll_event events[kMaxEvents];
      int n = ::epoll_wait(epfd_, events, kMaxEvents, -1);
      if (n == -1) {
        XLOGF_IF(ERR, errno != EINTR, "epoll_wait failed, errno {}, retry", errno);
        continue;
      }
      dispatch(events, n);
      lastWork = std::chrono::steady_clock::now();
    }

    if (processDeletes()) {
      seenVersion = ~0ull;
    }
  }

  XLOGF(INFO, "EventLoop::busyLoop() stopped.");
}

void EventLoop::dispatch(struct epoll_event *events, int n) {
  for (int i = 0; i < n; ++i) {
    auto &evt = events[i];
    if (evt.data.ptr == nullptr) {
      // waked up by event fd. read all.
      uint64_t val;
      while (::read(eventfd_, &val, sizeof(val)) > 0) {
      }
      continue;
    }

    auto wrapper = reinterpret_cast<HandlerWrapper *>(evt.data.ptr);
    if (auto handler = wrapper->handler.lock()) {
      handler->handleEvents(evt.events);
    }
  }
}

bool EventLoop::processDeletes() {
  if (deleteQueue_.empty()) {
    return false;
  }
  auto lock = std::unique_lock(mutex_);
  std::list<HandlerWrapper>::iterator it;
  // limit the number of deletions in a single iteration.
  for (auto i = 0ul; i < kDeleteQueueWakeUpLoopThreshold && deleteQueue_.try_dequeue(it); ++i) {
    wrapperList_.erase(it);
  }
  return true;
}

EventLoopPool::EventLoopPool(size_t numThreads)
    : eventLoops_(numThreads, nullptr) {
  for (auto &eventLoop : eventLoops_) {
    eventLoop = EventLoop::create();
  }
}

Result<Void> EventLoopPool::start(const std::string &threadName,
                                  BusyPollOptions busy,
                                  std::vector<uint32_t> cpus) {
  int idx = 0;
  for (auto &eventLoop : eventLoops_) {
    if (!cpus.empty()) {
      busy.cpu = cpus[idx % cpus.size()];
    }
    RETURN_AND_LOG_ON_ERROR(eventLoop->start(threadName + std::to_string(idx++), busy));
  }
  return Void{};
}

void EventLoopPool::stopAndJoin() {
  for (auto &eventLoop : eventLoops_) {
    eventLoop->stopAndJoin();
  }
}

Result<Void> EventLoopPool::add(const std::shared_ptr<EventLoop::EventHandler> &handler, uint32_t interestEvents) {
  auto &eventLoop = eventLoops_[folly::Random::rand32() % eventLoops_.size()];
  return eventLoop->add(handler, interestEvents);
}

}  // namespace hf3fs::net
