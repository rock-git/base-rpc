# RDMA Busy-Poll 设计方案

> 状态：第 3 节已实现（第 4 节共享 CQ 未做）。背景：bench（`proto=rdma concurrency=1 payload=64`）中 `network` 阶段 avg 53.3us，远高于 RoCE 硬件 RTT。

```
proto=rdma concurrency=1 payload=64 elapsed=10.00s
qps=15462
latency(us): avg=64.0 p50=63 p95=71 p99=75 p99.9=84
  client_pre_send          0.3
  network                 53.3
  server_queue             4.9
  server_process           0.2
  server_serialize         0.3
  client_post_recv         4.9
```

## 结论

能做，改动集中在 RDMA 路径，TCP 路径不用动。但光改成 busy-poll 只能去掉大约 1/3 的 network 耗时。慢的主要原因是线程切换太多，不是 epoll 本身。

## 1. "network" 阶段实际包含什么

时间戳的位置决定了统计口径：`clientSerialized` 在 `WriteItem::create` 里记，`serverReceived` 和 `clientReceived` 在 `Processor::unpackSerdeMsg` 里记。所以 "network" 统计的远不止网线上的时间。按默认配置（`rw_in_event_thread=false`），一个来回包括：

| # | 环节 | 类型 |
|---|---|---|
| H1 | client `send()` → `startWriteTask` → ioPool | 线程切换 |
| — | 线上传输 req | 网络 |
| I1 | server 硬件中断 → comp_channel → epoll 唤醒 EventLoop | 中断+唤醒 |
| H2 | EventLoop → `startReadTask` → ioPool | 线程切换 |
| H3 | `doRead` → `processMsg` → procPool（unpack） | 线程切换 |
| H4 | server 回包 `send()` → ioPool | 线程切换 |
| — | 线上传输 rsp | 网络 |
| I2 | client 中断 + epoll 唤醒 | 中断+唤醒 |
| H5, H6 | client 侧同 H2、H3 | 线程切换 |

**粗略估算（未单独测量）：**
- `server_queue` 和 `client_post_recv` 都正好是一次线程切换，各约 4.9us。按此推算，H1–H6 六次切换约 30us。
- I1、I2 两次中断加 epoll 唤醒约 10–16us。
- 线上 RTT 约 5–8us（bnxt_re RoCE 的估计值）。
- 合计约 50us，和实测的 53us 吻合。

**环境里还有两个放大因素：**
- 测试机是 E5-2650 v4，`intel_idle max_cstate=9`，深睡眠状态没关，每次唤醒都可能遇到较慢的退出延迟。
- 网卡 `bnxt_re0/1` 在 NUMA node0，而线程没有绑核。

## 2. 第 0 步：先做零代码实验

| 实验 | 方法 | 预期 |
|---|---|---|
| A | `--rw_in_event_thread=true` | 去掉 H1、H2、H4、H5（`startWriteTask` 会直接在调用线程里写），预计 −15~20us |
| B | 压测期间写 `/dev/cpu_dma_latency=0`，或执行 `cpupower idle-set -D 2` | 测出"唤醒慢"占多少，相当于 busy-poll 收益的上限参考 |
| C | `taskset`/`numactl -N 0` 把两端都绑到 node0 | 去掉跨 NUMA 的开销 |
| D | 安装 perftest，跑 `ib_send_lat` | 得到硬件的延迟下限 |

A 加 B 的结果可以直接告诉我们 busy-poll 值不值得做。

## 3. Busy-poll 方案

### 3.1 配置

放在 `IOWorker::Config`，用 `CONFIG_ITEM`，不支持热更新，避免运行时切换模式带来的竞态：

- `rdma_poll_mode`：`event`（现状，默认）、`busy`、`hybrid`
- `num_rdma_pollers`：poller 线程数，默认 1
- `busy_poll_idle_us`：hybrid 模式下空转多久后转入休眠，默认约 100us
- `rdma_poller_cpus`：绑核列表，应和网卡在同一 NUMA

### 3.2 Busy-poll 循环（实现为 `EventLoop` 的 busy 模式）

没有新建 `IBPoller` 类，而是给 `EventLoop` 加了 busy 模式（`EventLoop::start(name, BusyPollOptions)`），复用它已有的 handler 注册、weak_ptr 生命周期、`remove` 延迟删除和 epoll/eventfd。

- `EventHandler` 新增虚函数 `busyPoll(bool arm)`，默认返回 false；`Transport` 实现为直接 `IBSocket::pollCQ`。
- `IOWorker::start` 在非 EVENT 模式下额外创建 `rdmaPollers_`（`num_rdma_pollers` 个 busy 模式的 `EventLoop`），RDMA transport 经 `IOWorker::addToEventLoop` 加入它；TCP 仍走原 `eventLoopPool_`。
- 主循环（`EventLoop::busyLoop`）：

```
每轮: handler 列表有变化(add 会 bump version_, 删除后) → 加锁刷新快照
      pollAll(arm=false) 有活 → lastWork=now
      否则 BUSY 或未到 idle → pause 后继续空转
      否则 HYBRID 且空闲超过 idle → pollAll(arm=true)（先 req_notify 再 poll，防丢事件）
          有活 → 继续空转；没有 → epoll_wait(-1) → dispatch(handleEvents) → 回到空转
```

### 3.3 IBSocket / Transport 的改动

- `IBSocket::pollCQ(arm, polled)`：从原 `poll()` 拆出来，不碰 comp_channel；`arm=true` 时为原来的 poll → req_notify → poll 顺序。`poll()` = `cqGetEvent` + `pollCQ(!busyPoll_)`，busy 模式下被中断唤醒后不再重新 arm。
- `IBSocket::setBusyPoll()`：加入 busy 循环前由 `IOWorker::addToEventLoop` 设置。
- `Transport::busyPoll`：没有事件也没有错误时不进入 `onPolled`，省掉空转时的原子 `fetch_or`；transport 失效后仍继续 poll，让 flush 出来的 WC 唤醒等待中的 RDMA 操作。
- busy/hybrid 模式下 RDMA 的读写等同于 `read_write_rdma_in_event_thread=true`：读在 poller 线程内，写在调用线程内。
- 已知取舍：hybrid 睡眠前 arm 的 CQ，在被唤醒后继续空转期间仍保持 armed，每个睡眠周期每个 CQ 最多多一次无用中断。

### 3.4 正确性约束（和现有设计一致）

- **每个 socket 只由一个线程 poll。** 原来是一个 EventLoop，现在换成一个 poller。所以 `sendBufs_`、`recvBufs_`、`sendAcked_`、`sendSignaled_` 这些单生产者单消费者结构不用改。
- **关闭流程有一个坑。** `Drainer` 依赖 comp_channel 事件，但 busy 模式下 CQ 不会再被 arm，交接后收不到事件，会一直等到 `drain_timeout`（5s）超时。修法：在 `IBSocketManager::close` 里先 arm 一次、`drain()` 一次，再交给 `Drainer`。
- **单边 RDMA 不用特殊处理。** READ/WRITE 的 `RDMA_LAST` 完成事件走同一个 CQ，`baton.post()` 的逻辑不变。

### 3.5 一起做：去掉 proc 跳转（H3、H6）

- `Processor::Config::unpack_inline_max_size`（默认 0，即关闭）：一批收到的数据不超过这个大小时，直接在 IO/poller 线程里 `unpackMsg`。请求的 handler 仍然调度到 proc 线程池执行，响应仍然通过 `Waiter` 唤醒调用方。
- 代价是 poller 线程要承担反序列化，并发高时可能成为瓶颈，所以要做成可配置。

## 4. 扩展性（第 2 阶段，按需）

现在每个 QP 有自己的 CQ，空转一轮的开销随连接数线性增长。空 `ibv_poll_cq` 一次约 50–100ns，1000 条连接一轮约 50–100us，延迟反而会变差。

连接数多（比如每个 poller 超过 100 条）时，要改成每个 poller 共享一个 CQ（可选再加 SRQ），按 `wc.qp_num` 分发到对应 IBSocket。这会牵动：

- CQ 的归属和容量计算
- `Drainer`：不能再按 socket 单独 drain 一个共享 CQ

改动较大，先不做。

## 5. 预期收益和代价

| 阶段 | network 预估 |
|---|---|
| 现状 | 53us |
| + 实验 A（读写不切线程） | ~33us |
| + busy-poll | ~20us |
| + 小消息就地 unpack | ~10us |

以上都是推算，以实测为准。代价是 busy 模式下每个 poller 占满一个核，hybrid 模式空闲时占用很低。

## 6. 用法

```
# IOWorker::Config（Client: io_worker，Server: groups[i].io_worker），均为非热更新
rdma_poll_mode    = "EVENT" | "BUSY" | "HYBRID"
num_rdma_pollers  = 1
busy_poll_idle_us = 100      # 仅 HYBRID
rdma_poller_cpus  = []       # poller i 绑到 cpus[i % size]，建议和网卡同一 NUMA
# Processor::Config（热更新）
unpack_inline_max_size = 0   # 例如 "4KB"
```

bench：`--rdma_poll_mode=BUSY|HYBRID --busy_poll_idle_us=100 --rdma_pollers=1 --unpack_inline_max_size=4096`（server 和 client 都要加）。

## 7. 验证计划

- bench 测试矩阵：
  - 并发 1 / 64
  - payload 64B / 4KB / 64KB
  - 模式 event / busy / hybrid
  - 同时记录 qps、p99 和 CPU 占用
- `TestEventLoop.BusyPoll/HybridPoll`：用模拟 CQ（只有在 armed 状态下来活才写 pipe）验证 arm 竞态下不丢事件，BUSY 模式从不需要中断。
- `TestRDMABusyPoll.*` / `TestRDMAHybridPoll.*`（`test_base_rpc_ib`）：在 busy/hybrid 模式下跑 Concurrent、FaultInjectConcurrent、ServerDropConnections，覆盖 flush 错误和 Drainer 关闭路径。

## 8. 实测结果（2 × Xeon Gold 6330，mlx5 RoCE，10.220.68.25 → 68.26，payload 64B）

两台都是共享机器（load avg 约 23 / 9），数据有噪声。绑到网卡所在的 NUMA node1 反而更慢，可能因为 node1 上已有其他负载，所以下表都不绑核。

**concurrency=1（默认 1 连接，io/proc 线程各 2）**

| 模式 | qps | avg | p99 | network avg |
|---|---|---|---|---|
| EVENT | 13.8K | 71.8 | 88 | 60.2 |
| EVENT + rw_in_event_thread | 19.0K | 52.1 | 88 | 41.0 |
| BUSY | 28.3K | 34.7 | 65 | 24.2 |
| HYBRID (idle 100us) | 26.4K | 37.2 | 83 | 26.6 |
| BUSY + unpack_inline 4KB | 40.2K | 24.2 | 34 | 13.3 |
| HYBRID + unpack_inline 4KB | 39.3K | 24.8 | 35 | 13.9 |

**concurrency=64（4 连接，client bg/proc 线程 8、server proc 线程 8，pollers=4）**

| 模式 | qps | avg | p99 |
|---|---|---|---|
| EVENT | 1.03M | 61.2 | 84 |
| BUSY + unpack_inline 4KB | 0.86M | 73.0 | 173 |
| BUSY | 1.38M | 44.3 | 61 |

结论：

- 低并发下，BUSY/HYBRID 加 `unpack_inline` 把 network 阶段从约 60us 降到约 13us，端到端 p99 从 88us 降到约 34us。
- 高并发下 `unpack_inline` 会让 poller 线程卡在反序列化上，应该关掉；只开 BUSY 时吞吐比 EVENT 高 34%。
- 一个连接只由一个 poller 处理，而 `transport_pool.max_connections` 默认是 1。只有 1 条连接时，加 poller 没有用（c=64 下 BUSY 吞吐只有 EVENT 的约一半），高并发要同时调大连接数。
