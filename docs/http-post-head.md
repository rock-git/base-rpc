# HTTP POST / HEAD 支持设计方案

> 状态：已实现。在 [`http-sniff.md`](http-sniff.md) 的基础上，为同端口 HTTP 增加 `HEAD` 和 `POST`（含大 body）。不改变 RPC/RDMA 路径。

## 1. 目标与非目标

目标：
- 支持 `HEAD`：行为等同 `GET`，但响应不带 body，保留 `Content-Length`。
- 支持 `POST`：handler 能拿到请求体，body 上限默认 16MB。
- 现有只读页面保持只读，不会因此多出 POST 副作用。
- 能在读 body 之前拒绝的请求（404、405、413）一律在读 body 之前拒绝。

非目标：
- `Transfer-Encoding: chunked`（回 `501`）、流式 body、spill-to-disk。
- 大响应（仍受 `http-sniff.md` §6 风险 1 限制）。
- keep-alive、HTTPS、鉴权、gzip。
- `PUT`、`DELETE` 等其他方法（回 `405`）。

## 2. 现状

- `Processor::handleHttp`：非 `GET` 直接回 `405`。
- `HttpHandler = std::function<HttpResponse()>`，拿不到方法和请求体。
- 读缓冲 `readBuff_` 固定 32KB（`kMessageReadBufferSize`），请求头收全后才解析，缓冲满且没有 `\r\n\r\n` 回 `431`。
- 每个连接只处理一个请求，响应后关闭。

## 3. 决策记录

| # | 问题 | 决策 |
|---|---|---|
| 1 | handler 如何区分方法、拿 body | `HttpHandler` 改为 `function<HttpResponse(const http::Request &)>`；路由表仍按 path 一张表；HEAD 当 GET 调 handler，由 `Transport` 在序列化后去掉 body |
| 2 | body 怎么收 | 只支持 `Content-Length`；无 `Content-Length` 的 POST 回 `411`；`Transfer-Encoding` 回 `501` |
| 3 | 大 body 放哪、上限 | 连续内存，`req.body` 仍是 `string_view`；缓冲满时按倍增扩容，上限为 `headerSize + contentLength`，不按声明长度一次性分配；热更新配置 `http_max_body_size`，默认 16MB，超出回 `413` |
| 4 | `Expect: 100-continue` | 支持：路由、方法、大小检查都通过后才回 `100 Continue`，每连接只发一次；HTTP/1.0、无 body 或已收到部分 body 时不发；其他 `Expect` 值回 `417` |
| 5 | 只读页面收到 POST | 默认只读：`addHttpRoute(path, handler, bool allowPost = false)`，不允许时回 `405` + `Allow` |
| 6 | 404/405 何时判断 | 请求头收全后立即判断（`Processor::checkHttp`），不等 body |
| 7 | 回错误并断连时的 RST 问题 | 不特殊处理，列为已知限制 |

## 4. 详细设计

### 4.1 `http` 模块（`common/net/http/Http.{h,cc}`）

- `Request` 新增：
  - `size_t headerSize`：请求行加头部的长度（含结尾 `\r\n\r\n`），body 从这里开始。
  - `size_t contentLength`：无 `Content-Length` 时为 0。
  - `bool expect100`：`Expect: 100-continue` 且版本为 `HTTP/1.1`。HTTP/1.0 不回 100（RFC 9110 §10.1.1）。
  - `std::string_view body`：指向读缓冲，和其他字段一样，handler 返回前有效。
- `Response` 新增 `std::string allow`；`formatResponse` 非空时输出 `Allow: ...\r\n`。
- `reasonPhrase` 补：`100`、`411`、`413`、`417`、`501`。
- `parseRequest` 在请求头收全后解析头部。头名和 `Expect` 值都大小写不敏感，头值去掉前后空白：

| 情况 | 返回 |
|---|---|
| `Content-Length` 非法（空、非数字、带 `+`/`-`、溢出、重复且不一致；用 `std::from_chars`） | `400` |
| 存在 `Transfer-Encoding` | `501` |
| `Expect` 不是 `100-continue` | `417` |
| POST 无 `Content-Length` | `411` |
| 头已收全，body 未收全 | `0`，填好除 `body` 外的字段 |
| 全部收全 | `200`，填好全部字段，`body` 长度恰为 `contentLength`（之后的多余字节忽略） |

- 调用方用 `req.headerSize != 0` 区分返回 `0` 时的两种情况："头未收全"和"头已收全、body 未收全"。
- `bufferFull` 语义不变：只在头未收全时生效（回 `431`）。头已收全后由 `Transport` 扩容，保证不会再出现缓冲满。
- `http_max_body_size` 由 `Transport` 判断（`Http` 模块无配置、无 I/O）。

### 4.2 `Processor`（`Processor.h`、`ServiceGroup.h`）

- 配置：`CONFIG_HOT_UPDATED_ITEM(http_max_body_size, 16_MB)`，按 group 生效。
- `addHttpRoute(std::string path, HttpHandler handler, bool allowPost = false)`；`ServiceGroup` 同步转发。
- 路由表值改为 `{HttpHandler handler; bool allowPost;}`。
- `std::optional<HttpResponse> checkHttp(const http::Request &req) const`，请求头收全后调用：
  1. 路径未注册且不是 `/` → `404`（任何方法）。`/` 未注册时为索引页，按只读路由处理。
  2. 方法不在允许范围（GET、HEAD，加上 `allowPost` 时的 POST）→ `405`，`Allow` 填 `GET, HEAD` 或 `GET, HEAD, POST`。
  3. 否则返回 `nullopt`。
- `HttpResponse handleHttp(const http::Request &req) const`：body 收全后调用，假定已通过 `checkHttp`。调对应 handler 或生成索引页，HEAD 和 GET 走同一路径。
- `HttpHandler` 签名变化会影响所有现有注册点：`Server.cc` 默认页面、`examples/http_server.cc`、`TestHttp.cc`。

### 4.3 `Transport`（`Transport.{h,cc}`）

每次 `doHttp()` 都重新调用 `parseRequest`（只扫描到头部结尾，代价小）。

请求头收全后（返回 `0` 且 `headerSize != 0`，或返回 `200`）：

1. `processor.checkHttp(req)` 有值 → 回该响应并断连。
2. `contentLength > http_max_body_size` → 回 `413` 并断连。即使 body 已经收全也检查，配置可能小于 32KB。
3. 返回 `200`：调 `handleHttp`，发送响应，流程见下文。
4. body 未收全时：
   1. `req.expect100 && !sent100_ && readBuff_->length() == req.headerSize` → `socket_->send` 一次 `HTTP/1.1 100 Continue\r\n\r\n`，置 `sent100_ = true`。已经收到部分 body 时不发（客户端已经在发了）。
   2. `readBuff_->tailroom() == 0` → 新容量 `min(2 * capacity, headerSize + contentLength)`，分配新的 `IOBuf`，拷贝已读数据，替换 `readBuff_`。由于 body 未收全，新容量一定大于已读长度，扩容后 tailroom > 0，不会出现 recv 读 0 字节被当成 `EAGAIN` 而挂住。
   3. 返回 `false`，继续读。读循环沿用 `http-sniff.md` §4.1 的约束：必须回到 `recv/tryToSuspend`，不能直接从 `doRead` 返回（`EPOLLET` + `kReadAvailableFlag`）。

发送响应：`text = formatResponse(rsp)`；`req.method == "HEAD"` 时执行 `text.resize(text.size() - rsp.body.size())`，`Content-Length` 保持不变，错误响应同样适用。之后的一次 `send`、`invalidate(false)`、`kLastReadFinished` 不变。

`Transport` 新增成员：`bool sent100_ = false`。

### 4.4 示例

`examples/http_server.cc` 增加 POST 路由 `/echo`（`allowPost = true`）。返回 `"<body 长度> <body 前 64 字节>\n"`，响应保持很小，绕开发送缓冲的限制（`http-sniff.md` §6 风险 1）。用于 `curl -d`、`curl -I` 验证。

## 5. 改动清单

| 文件 | 改动 |
|---|---|
| `net/http/Http.{h,cc}` | `Request.headerSize`/`contentLength`/`expect100`/`body`、`Response.allow`、头部解析、新状态码 |
| `net/Processor.h` | `http_max_body_size`、路由表值结构、`allowPost`、`checkHttp`、`handleHttp` 重写 |
| `net/ServiceGroup.h` | `addHttpRoute` 转发新参数 |
| `net/Transport.{h,cc}` | `sent100_`、提前检查 404/405/413、`100 Continue`、倍增扩容、HEAD 去 body |
| `net/Server.cc` | 默认页面 handler 改签名 |
| `examples/http_server.cc` | handler 改签名，新增 `/echo` |
| `tests/common/net/TestHttp.cc` | 见 §7 |
| `docs/http-sniff.md`、`README.md`、`docs/architecture.md` | 更新非目标和方法说明，链接本文 |

## 6. 风险与限制

1. **被拒绝请求的 RST**：`413`、`411`、`501`、`417`、`404`、`405` 在 body 未读完时回复并断连。内核接收缓冲里还有未读数据时 `close` 会发 RST，客户端可能收不到响应（`Connection reset by peer`）。带 `Expect: 100-continue` 的客户端不受影响，小 body 通常已被读走也不受影响。不做 drain / 延迟关闭，建议大 body 客户端使用 `Expect: 100-continue`。
2. **慢速大 body 连接的内存**：倍增扩容后，每个连接占用的内存不超过 `max(32KB, 2 × 已收字节)`。只声明大 `Content-Length` 而不发数据的连接只占 32KB。但持续慢速发送的连接仍会持有内存，直到 `connection_expiration_time`（默认 1 天）。多个连接叠加时，总量受 `http_max_body_size × 连接数` 限制。无鉴权，暴露面同 RPC 端口；必要时调小 `http_max_body_size`。
3. **handler 线程**：与 `http-sniff.md` §6 风险 2 相同，handler 可能在 epoll 线程运行，必须非阻塞。POST handler 处理 16MB body 时尤其要注意耗时。
4. **扩容拷贝**：倍增扩容的总拷贝量小于最终 body 大小，16MB body 约扩容 9 次（32KB → 16MB）。
5. **响应大小**：`POST` 响应仍要小于 socket 发送缓冲（`http-sniff.md` §6 风险 1），handler 不要回显大 body。
6. **上游同步冲突**：`Transport.*`、`Processor.h`、`ServiceGroup.h`、`Server.cc` 来自 3FS，改动需在 README 的 "Differences from 3FS" 表里登记。

## 7. 测试计划

`tests/common/net/TestHttp.cc`：

解析（`TestHttpParser`）：
1. `Content-Length` 合法、头名大小写混用、值带前后空白；非法值（空、`-1`、`+1`、`abc`、溢出、重复不一致）→ `400`；重复且一致 → 通过。
2. `Transfer-Encoding: chunked` → `501`。
3. `Expect: 100-continue`（含大小写混用）→ HTTP/1.1 下 `expect100 = true`，HTTP/1.0 下为 `false`；其他值 → `417`。
4. POST 无 `Content-Length` → `411`。
5. 头未收全 → `0` 且 `headerSize == 0`；body 未收全 → `0` 且 `headerSize`/`contentLength` 正确；收全 → `200` 且 `body` 正确，多余字节不计入。
6. `formatResponse` 带 `Allow` 头。

集成（TCP `Server`，不依赖 RDMA）：
1. HEAD：状态行和头与 GET 一致，没有 body，`Content-Length` 等于 GET 的 body 长度；HEAD 未注册路径 → `404` 且没有 body。
2. POST `/echo`：小 body、分两次发送的半包、1MB body（验证倍增扩容，校验返回的长度和前缀）。
3. 超过 `http_max_body_size` → `413`（用 `Expect: 100-continue` 流程测，不应收到 `100`）；配置调小到 1KB 时，一次发完的 2KB body 也回 `413`。
4. POST 无 `Content-Length` → `411`；chunked → `501`。
5. 只读路由收到 POST → `405` 且 `Allow: GET, HEAD`；`PUT`/`DELETE` → `405`；带 `Expect: 100-continue` 的 POST 打到只读路由或未注册路径时，直接回 `405`/`404`，不回 `100`。
6. `Expect: 100-continue`：先收到 `100 Continue`，再发 body，最终 `200`；每个连接只收到一次 `100`。
7. 现有用例不变，`TestEcho`、`TestProcessor`、`TestService` 回归通过。
8. RPC 与 HTTP 在同一端口共存不受影响。

## 8. 落地步骤

1. `Http.{h,cc}`：`Request`/`Response` 字段、头部解析、状态码，配 `TestHttpParser`。
2. `Processor`/`ServiceGroup`：handler 签名、`allowPost`、`http_max_body_size`、`checkHttp`/`handleHttp`；同步改 `Server.cc`、示例、现有测试。
3. `Transport`：提前检查、`100 Continue`、倍增扩容、HEAD 去 body；通过 §7 集成用例。
4. 示例加 `/echo`，更新 `http-sniff.md`、README、`docs/architecture.md`。
