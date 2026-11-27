# base-rpc

[English](README.md) | 中文

从 [3FS](https://github.com/deepseek-ai/3FS)（MIT，见 `LICENSE`）中抽取的网络与 RPC 库：
`common/net`（TCP / RDMA / unix 传输、事件循环、服务端、客户端）和 `common/serde`
（序列化及 `SERDE_SERVICE` RPC 框架），以及它们依赖的少量 `common/utils` 代码。

导出自 3FS 提交 `UPSTREAM_COMMIT`。头文件路径（`common/...`）和 `hf3fs::` 命名空间保持不变，
因此线上协议与 3FS 相同，3FS 的修复可以通过 `scripts/sync_upstream.sh` 重新应用。

## 构建

依赖：**gcc 13+ 或 clang**（已测试：gcc 13 完整构建 + 测试，gcc 14 单个编译单元，clang 21；gcc 15 在 `CallContext.h` 处触发编译器内部错误），CMake >= 3.12，
Boost（filesystem、system、program_options，以及 folly 所需的其他组件），glog、gflags、double-conversion、libevent、
OpenSSL，以及 rdma-core 头文件/库（`libibverbs`，当前版本即使只用 TCP 也需要）。
folly、fmt、scnlib、toml11、zstd 和 googletest 为 git 子模块（`git submodule update --init`）；folly 在
配置阶段会自动应用 `patches/folly.patch`。

```sh
cmake -S . -B build -G Ninja            # 使用 clang 时加上 -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake --build build
ctest --test-dir build
./build/examples/echo
```

注意：
- gcc 编译很慢：每个包含 net/serde 头文件的编译单元需要 30 秒以上（clang 只需几秒）。
- （clang）固定版本的 fmt（10.0.0-71）与 clang >= 19 的 consteval 不兼容：加上 `-DCMAKE_CXX_FLAGS=-DFMT_CONSTEVAL=`。
- （clang）如果 clang 选用了过新的 libstdc++（gcc 15 会在 folly `OrderedMap` 处失败），加上 `--gcc-install-dir=<gcc 13 目录>`。
- 如果系统中的 fmt/folly 在 include 路径中排在 `third_party/fmt` 之前，请将 `CMAKE_PREFIX_PATH` 指向不包含它们的前缀。

## 在其他项目中使用

```cmake
add_subdirectory(third_party/base-rpc)       # 或使用 FetchContent
target_link_libraries(my_app PRIVATE base-rpc)
```

- 父项目可以自行提供 `folly`、`fmt`、`scn::scn`、`toml11`、`libzstd_static` 目标；此时 base-rpc
  会跳过对应的子模块副本。所提供的 folly 必须包含 3FS 的补丁。
- 在 x86_64 上所有代码都要用 `-msse4.2 -mavx2` 编译（folly F14 容器依赖于此；base-rpc 仅在作为
  顶层项目时才会设置该选项）。
- 服务端 + 客户端示例见 `examples/echo.cc`。服务 id 与错误码：base-rpc 保留了 3FS 的
  `StatusCodeDetails.h`，新增错误码时请避免与已有错误码冲突。
- TCP 和 unix socket 无需 RDMA 硬件即可使用；`Address::RDMA` 需要 RDMA 网卡。

## 与 3FS 的差异

复制的文件除以下改动外保持不变。gcc 的兼容性改动都放在 `#ifdef __clang__` 之后，因此 clang 编译的是 3FS 原始代码：

| 文件 | 改动 |
|---|---|
| `common/serde/Serde.h`、`common/net/ib/RDMABuf.h` | `#ifdef __clang__` 下保留 3FS 的 `struct ::ns::SerdeMethod<...>` 写法；其他编译器使用去掉前导 `::` 的写法（gcc 不接受） |
| `common/serde/Serde.cc` | `Out<JsonObject/TomlObject>::add` 的定义：clang 保留原写法，gcc 使用 `template <> template <class U> ... add(U &&v)`（gcc 不接受简写形式） |
| `common/serde/Services.h` | clang 保留 `std::array`；其他编译器的 `services_` 改用 `std::vector`（gcc 会把数组构造展开成 5MB 的构造函数，编译耗时数分钟） |
| `common/serde/SerdeHelper.h` | 删除了 `store/load/snapshotLoad`（KV 事务辅助函数） |
| `common/net/Server.{h,cc}` | 去掉 `AppInfo`：`start()` 不带参数，没有 `appInfo()` |
| `common/net/{Transport,Processor,ServiceGroup,IOWorker,TransportPool}.h`、`Transport.cc`、`Server.cc` | 同端口 HTTP 状态页（`processor.enable_http`，默认关闭），见 `docs/http-sniff.md`；HEAD/POST（`processor.http_max_body_size`），见 `docs/http-post-head.md` |
| `common/net/http/Http.{h,cc}`、`tests/common/net/TestHttp.cc` | 新增 |
| `common/net/ServiceGroupInfo.{h,cc}` | 新增，`flat::ServiceGroupInfo` 复制自 `common/app/AppInfo.h` |
| `common/monitor/{Recorder,Sample}.h` | **shim**：所有指标均为空操作 |
| `memory/common/GlobalMemoryAllocator.h` | **shim**：malloc/free/posix_memalign |
| `compat/gtest/gtest_prod.h` | 无需安装 gtest 即可使用 `FRIEND_TEST` |
| `tests/common/serde/TestSerde.cc` | 仅 gcc：在宏内 lambda 中给 `a > 0` 加括号（gcc 解析 bug） |
| `tests/main/TestMain.cc` | 简化（无 3FS 日志处理器 / 监控） |
| `tests/common/net/ib/SetupIB.h`、`TestEcho.cc`、`TestProcessor.cc`、`TestRDMAControl.cc`、`TestService.cc` | 没有 RDMA 设备时跳过 RDMA 测试 |

未移植：monitor、`common/app`、`common/kv`、`fbs`、`GitVersion` / 版本信息（`VersionInfo.h` 仅被
include，从未被调用）。

## 脚本

- `scripts/gen_paths.py <3FS>`：重新计算导出文件列表（`scripts/paths.txt`）。
- `scripts/export.sh <3FS>`：首次复制（会覆盖上述已修改的文件，首次导出后不要再运行）。
- `scripts/sync_upstream.sh <3FS> [commit]`：将上游改动应用到已导出的文件。
- `scripts/check_deps.py`：如果有 include 指向本仓库之外则报错。

## RDMA 测试

`tests/common/net/ib` 需要真实的 RDMA 网卡：配置时加上 `-DBASE_RPC_RUN_IB_TESTS=ON` 以将其注册到 ctest。
