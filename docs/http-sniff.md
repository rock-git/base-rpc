# 同端口 HTTP（仅 TCP）设计方案

> 状态：已实现（`Transport::doHttp`，测试 `tests/common/net/TestHttp.cc`）。目标：在现有 RPC 的 TCP 监听端口上直接提供只读 HTTP 页面，用浏览器 / curl 查看服务内部状态；不开新端口，不碰 RDMA 数据路径。

## 1. 目标与非目标

目标：
- 同一个 TCP 端口上，RPC 与 HTTP/1.1 `GET` 共存，靠连接的第一个字节区分。
- RDMA 部署也可用：`Listener` 的 RDMA 模式仍用 TCP socket 接入（`IBConnectService` 走 TCP），HTTP 走这些 TCP 连接，RDMA 数据路径不受影响。
- 默认关闭；handler 以 `path → 函数` 的形式注册，`Server` 提供若干默认状态页。

非目标（本期不做）：
- 不支持 RDMA transport 上的 HTTP（RDMA 连接只走 serde）。
- 不支持 UNIX domain socket 上的 HTTP。
- 不做 keep-alive、chunked、HTTPS、鉴权、gzip。`HEAD` 和 `POST` 见 [`http-post-head.md`](http-post-head.md)。
- 不做大响应（见 §6 风险 1）。

## 2. 现状

- 线协议：8 字节头 `{uint32 checksum, uint32 size}` + serde 包。`checksum` 的低 8 位固定为 `0x86` 或 `0x87`（`MessageHeader::isSerdeMessage`，`kSerdeMessageMagicNum = 0x86`，最低位是 compressed 标志）。x86 小端下，这个字节是连接上的**第一个字节**。
- `Listener::acceptTCP` 把 fd 交给 `IOWorker::addTcpSocket`，构造 `Transport` 并注册到 epoll（`EPOLLIN|EPOLLOUT|EPOLLET`）。TCP 连接的 `peerAddr().type` 为 `TCP`。
- `Transport::doRead` 读到数据后直接按 `MessageHeader` 解析，`size >= kMessageMaxSize(512MB)` 就 `tryToCleanUp`。HTTP 请求（如 `"GET /ab"`）当前会被当成一个超大消息而断连。
- 写路径：`WriteItem` 绑定 `SerdeBuffer`（含 8 字节 serde 头），不能直接装一段 HTTP 文本。
- 配置：`Processor::Config` 是 `ServiceGroup::Config` 下每个 group 一份，`Transport` 通过 `ioWorker_` 可达 `IOWorker::processor_`。

## 3. 协议判别

| 首字节 | 判定 |
|---|---|
| `0x86` / `0x87` | serde RPC |
| 其他 | HTTP（非法请求由 `doHttp` 回 `400` 后断连） |

判别只看第 1 个字节：`(b & 0xfe) == kSerdeMessageMagicNum ? Serde : Http`，不需要缓冲多字节。HTTP 方法首字母均为 ASCII（< `0x80`），不会误判为 serde。

判别发生在每个**服务端接入的 TCP `Transport`** 的第一次 `doRead` 之后。`Transport` 新增成员 `proto_`：`Unknown`（初值）、`Serde`、`Http`。`proto_` 只在读任务里访问，而读任务全局只有一个在跑，所以不需要原子变量。

适用范围：
- 仅当 `isTCP() && !serverAddr_.isUNIX()`（RDMA 连接的 `proto_` 初值即为 `Serde`）且 `processor.enable_http()` 为 true 时才判别；否则 `proto_` 直接置为 `Serde`，行为与现在完全一致。
- 客户端主动 `connect` 出去的 `Transport` 不做特殊处理：它只会收到 serde 回复，首字节必为 `0x86/0x87`，判别结果自然是 `Serde`。

## 4. 详细设计

### 4.1 `Transport`（`Transport.h/.cc`）

`doRead` 在 `readBuff_->append(readSize)` 之后、`MessageWrapper msgWrapper(...)` 之前加：

```cpp
if (UNLIKELY(proto_ != Proto::Serde)) {
  if (proto_ == Proto::Unknown) {
    proto_ = sniff(*readBuff_);          // 看首字节，按 §3 判别
  }
  if (proto_ == Proto::Http) {
    if (!doHttp()) continue;             // 请求头不完整：继续读，recv 返回 0 后走 tryToSuspend
    return;                              // 已回复并关闭
  }
}
```

热路径多一次 `proto_ != Serde` 比较，开销可忽略。

注意：请求头不完整时**必须回到读循环**，不能直接从 `doRead` 返回。`kReadAvailableFlag` 只在 `tryToSuspend` 中清除，直接返回会让该标志常驻，`onPolled` 不再启动读任务；而 epoll 为边沿触发（`EPOLLET`），后续数据到达也不会再通知，连接会永久挂起。

协议的字节级处理独立在 `common/net/http/Http.{h,cc}`（命名空间 `net::http`，无 I/O、无路由，见 §4.4），`Transport` 只负责收发与连接状态。

`doHttp()` 返回 `bool`（true = 已回复并关闭，false = 需要更多数据），行为：
1. `http::parseRequest(buf, tailroom()==0, req)`：返回 `0` 表示请求头未收全，`doHttp` 返回 false；缓冲区满（32KB）仍无 `\r\n\r\n` 返回 `431`（缓冲区满时 `recv` 返回 0，若不处理会被误当作 EAGAIN 挂起）；请求行格式错返回 `400`；成功返回 `200` 并填好 `http::Request`。
2. 解析成功交给 `ioWorker_.processor_.handleHttp(req)`（`Transport` 是 `IOWorker` 的 friend，见 §4.2）：方法不是 `GET` 回 `405`，未注册路径回 `404`（后续扩展了 `HEAD`/`POST`，见 [`http-post-head.md`](http-post-head.md)）；失败则 `http::Response::error(status, reasonPhrase)`。
3. `http::formatResponse(rsp)` 生成 `HTTP/1.1 <status>\r\nContent-Type: ...\r\nContent-Length: ...\r\nConnection: close\r\n\r\n<body>`，在 `doHttp` 里直接一次 `socket_->send` 发出，然后关闭：
   ```cpp
   invalidate(false);                                        // 不打 WARNING 日志
   wakeUpAfterLastReadAndWriteFinished(flags_ |= kLastReadFinished);
   ```
   不用 `tryToCleanUp(false)`：它内部调用 `invalidate()`（`logError=true`），每个 HTTP 请求都会在写任务里打一条 WARNING。

发送：直接调用 `socket_->send(iov, len)`（非阻塞，未单独抽 `sendRaw` 函数）。一次没发完（记一条 WARNING）（`EAGAIN` 或部分写）就放弃并断连。这是已知限制，代码里按 `shortcut: raw send, response must fit in the socket send buffer (tens of KB on default tcp_wmem); switch to a WriteItem variant for large pages` 注释。

线程：HTTP 连接上只有读任务（响应由读任务内联发送），不与 RPC 写路径竞争。`read_write_tcp_in_event_thread=true` 时这段代码会在 epoll 线程里跑，所以 handler 必须是非阻塞的（见 §4.2、§6 风险 2）。

### 4.2 `Processor`（`Processor.h`）

新增：
- 配置项 `CONFIG_HOT_UPDATED_ITEM(enable_http, false)`，放在 `Processor::Config`，按 group 生效。
- 成员 `std::map<std::string, HttpHandler, std::less<>> httpRoutes_`，`using HttpResponse = http::Response`，`using HttpHandler = std::function<HttpResponse()>`。
- `void addHttpRoute(std::string path, HttpHandler)`：只允许在 group `start()` 之前调用，运行期只读，所以不需要加锁。
- `HttpResponse handleHttp(const http::Request &)`：非 `GET` 返回 405；按 `req.path` 查表，未命中返回 404；`/` 未注册时返回路由列表（索引页由 `Processor` 生成）。
- 路由表用 `std::map<std::string, HttpHandler, std::less<>>`：可用 `string_view` 查找，索引页按路径排序。
- `bool httpEnabled() const`。

`ServiceGroup` 增加转发方法 `addHttpRoute(...)`。`Transport` 是 `IOWorker` 的 friend，可直接访问 `processor_`，不需要新增 `processor()` 访问器；但 `/connections` 需要读 `pool_`，`IOWorker` 要加一个 `pool()` 只读访问器。

### 4.3 `Server`（`Server.cc`）

在 `Server::start()` 中 `beforeStart()` **之前**，对每个 group 注册默认页面（此时监听尚未开始，满足"启动前注册"；应用在 `beforeStart()` 中注册同名路径即可覆盖默认页面）：

| 路径 | 内容 | 数据来源 |
|---|---|---|
| `/` | 页面索引 | 路由表（`Processor::handleHttp` 内置） |
| `/server` | 服务列表、监听地址 | `Server::describe()` |
| `/connections` | 连接总数；前 N 条（默认 200）连接的对端、是否失效、最近使用时间 | `TransportPool`（需加只读遍历接口）、`Transport`（需加 `lastUsedTime()` getter） |

`/connections` 只列前 N 条，是为了让响应留在 socket 发送缓冲以内（见 §6 风险 1）。`/processor`、`/threads` 本期不做。

应用层（继承 `Server` 的服务）可以通过 `groups()` 向任意 group 注册自己的页面，**但只能在 `beforeStart()` 中注册**；运行期注册会与读任务查表产生数据竞争。

### 4.4 `http` 模块（`common/net/http/Http.{h,cc}`）

纯函数，不依赖 net 其他头文件，可单独测试（`TestHttpParser`）：

| 接口 | 说明 |
|---|---|
| `struct Request` | `method`、`target`、`path`、`query`、`version`、`headers`（请求行之后的原始头部行，未解析），均为指向读缓冲的 `string_view` |
| `struct Response` | `status`、`contentType`、`body`；`Response::error(status, msg)` |
| `int parseRequest(buf, bufferFull, req)` | 返回 HTTP 状态：`0` 未收全，`200` 成功，`400`/`431` 错误 |
| `std::string formatResponse(rsp)` | 带 `Content-Length`、`Connection: close` |
| `std::string_view reasonPhrase(status)` | 状态码文字 |

后续扩展（解析请求头、keep-alive、POST body、把 `Request` 传给 handler 等）应在此模块内完成，`Transport` 只需按 `parseRequest` 的返回值决定继续读还是回包。

### 4.5 配置示例

```toml
[[server.groups]]
[server.groups.processor]
enable_http = true      # 默认 false
```

## 5. 改动清单

| 文件 | 改动 | 约行数 |
|---|---|---|
| `net/Transport.{h,cc}` | `proto_`、判别、`doHttp`、`lastUsedTime()` getter | 75 |
| `net/http/Http.{h,cc}` | 新增：请求解析、响应序列化 | 90 |
| `net/Processor.h` | `enable_http`、路由表、`handleHttp`（含 GET 检查） | 30 |
| `net/IOWorker.h` | `pool()` 只读访问器 | 2 |
| `net/ServiceGroup.h` | `addHttpRoute` 转发 | 3 |
| `net/TransportPool.h` | 连接信息只读遍历 `forEach` | 11 |
| `net/Server.cc` | 默认页面注册 | 20 |
| `tests/common/net/TestHttp.cc` + `CMakeLists` | 见 §7 | 60 |
| `README.md`、`docs/architecture.md` | 差异表、说明 | 10 |

合计约 215 行，其中只有判别和 `doHttp` 位于读路径。

## 6. 风险与限制

1. **响应大小**：响应写不完就断连，页面必须小于 socket 发送缓冲。Linux 默认 `tcp_wmem[1]=16KB`，新建连接的发送缓冲通常只有几十 KB，因此 `/connections` 只列前 N 条，应用自定义页面也应控制在几十 KB 内。注意：调大过 `tcp_wmem` 的机器（如开发机为 16MB）上测试不出截断。后续若需要大页面，给 `WriteItem` 加 raw 变体，并改 `toIOVec`/`advance`。
2. **handler 线程**：`doRead` 可能运行在 epoll 线程（`read_write_tcp_in_event_thread`）。handler 必须非阻塞、不持长锁；读 `TransportPool` 等需要使用已有锁或做快照拷贝，不能长期持锁。
3. **安全**：无鉴权，且与 RPC 端口同暴露面。因此：默认关闭；handler 只读；请求头上限 32KB；只处理一条请求就关连接；可用 `Transport::peerIP()` 加来源白名单（本期不做，列为后续）。在 RDMA 部署里，TCP 端口通常在内网网卡上，仍应在部署时确认暴露范围。
4. **上游同步冲突**：`Transport.cc`、`Processor.h`、`IOWorker.h`、`ServiceGroup.h`、`Server.cc`、`TransportPool.*` 都是从 3FS 拷贝的文件。要在 README 的 "Differences from 3FS" 表里逐项登记。`sync_upstream.sh` 对这些文件可能产生冲突，需要手动合并。
5. **字节序**：判别基于 `checksum` 低字节是线上的首字节，仅对小端成立（项目本身只面向 x86，见 README 的 `-msse4.2 -mavx2`）。
6. **半包与粘包**：RPC 判别只依赖第一个字节，不受 TCP 分片影响；但若一个 RPC 客户端在首包之前发了非 `0x86/0x87` 的字节，会被当作 HTTP 处理并回 400，之后断开，与此前"校验失败断连"的结果一致。
7. **半关闭客户端**：`TcpSocket::recv` 读到 EOF 时直接返回错误，同一次调用里已读到的数据会被丢弃。发完请求立即关闭写端的客户端（如 `nc -N`、部分脚本）收不到响应；curl、浏览器不受影响。本期不改 `recv`。
8. **空闲连接**：未发完请求头的 HTTP 连接最多占 32KB 读缓冲，直到 `connection_expiration_time`（默认 1 天）才被清理。现有 RPC 连接同样如此，只是暴露面变大；列为已知风险，必要时调小该配置。
9. **编译时间**：gcc 下每个包含 net 头文件的编译单元要 30 秒以上，`Transport.cc` 改动后重编较慢；新增代码尽量不引入新的重头文件。

## 7. 测试计划

`tests/common/net/TestHttp.cc`，不依赖 RDMA 设备：
1. 起一个 TCP `Server`，开 `enable_http`，注册路由 `/ping`。
2. 用原始 socket 发 `GET /ping HTTP/1.1\r\n\r\n`，断言返回 `200` 和预期 body。
3. 同一端口再做一次 echo RPC，断言成功（证明共存）。
4. 未注册路径 → `404`；`POST /ping` → `405`；超过 32KB 无 `\r\n\r\n` 的数据 → `431`；请求分两次发送且中间停顿（半包，验证不会挂起）→ `200`。
5. `enable_http=false`（默认）时发 HTTP 请求，连接被关闭（`"GET / HTTP/1.1"` 的第 5~8 字节被解析成约 1.4GB 的长度，超过 512MB 上限），且不影响 RPC。
6. 现有 `TestEcho`、`TestProcessor`、`TestService` 回归通过。

## 8. 落地步骤

1. `Processor` 加配置与路由表，`ServiceGroup` 加转发，`IOWorker` 加 `pool()` 访问器。
2. `Transport` 加判别与 `doHttp`，先支持固定页面，通过 §7 的 1~5。
3. `TransportPool` 加只读遍历、`Transport` 加 getter，`Server` 注册 `/`、`/server`、`/connections`。
4. 更新 README 差异表与 `docs/architecture.md`。
5. （可选后续）来源 IP 白名单、`WriteItem` raw 变体以支持大响应、`/processor`、`/threads` 页面、简单的 JSON 输出。
