## 实施状态（2026-11-24）

已完成 3.1–3.4 和第 2 节的依赖切断：`cmake --build` 全量通过，`ctest` 85 个用例通过（TCP/UNIX 路径，无 RDMA 网卡的机器上
RDMA 用例自动 skip），`examples/echo` 跑通，`scripts/check_deps.py` 通过。与方案不同的地方：

- 没有 filter-repo 导出历史，直接按 `scripts/paths.txt` 拷贝（`scripts/export.sh`）。
- 不带 `GitVersion.cmake` / `VersionInfo.cc.in`：`VersionInfo` 在拷贝的代码里只被 include、从未调用。
- 不带 `Target.cmake`，顶层 `CMakeLists.txt` 自己定义 `base-rpc` 目标。
- 补充的文件：`common/utils/Toml.cc`（Toml.hpp 的实现，只被 .hpp 配对，原脚本漏掉）、`compat/gtest/gtest_prod.h`。
- 已支持 gcc 13（全量编译 + ctest 通过）和 clang；gcc 15 编译 CallContext.h 会内部错误（ICE）。改动见 README 差异表；`-msse4.2 -mavx2` 需要和 folly 一致，由顶层工程全局添加。
- 测试：IB 目录单独成 `test_base_rpc_ib`，默认只编译不注册 ctest（`-DBASE_RPC_RUN_IB_TESTS=ON` 才注册）。
- 未做：真实 RDMA 机器上的测试（本机没有可用 RDMA 设备）、纯 TCP 构建（仍需 libibverbs）、install/find_package。

构建环境相关的坑（clang 21 + 旧 fmt、libstdc++ 版本、系统 fmt 抢先）记录在 README。

---

# base-rpc 抽离计划

## 目标与约束

- 把 3FS 的网络和 RPC 代码（`src/common/net`、`src/common/serde` 及其必要的基础工具）抽成独立库 `base-rpc`，供其他项目使用。
- **3FS 项目本身不做任何修改**。这是单向导出，3FS 继续独立演进。
- **监控（metrics）代码不迁移**，在 base-rpc 里用空实现替代。
- 来源基线：3FS `2dcbff5`，记录在 `UPSTREAM_COMMIT` 文件里。
- 许可：3FS 是 MIT 许可，base-rpc 保留原 `LICENSE` 版权声明，并在 README 注明来源。

## 原则

1. **尽量不改拷贝过来的源文件**。能用替代头文件（shim）解决的，就不改原文件，方便以后从 3FS 合并修复（保持相同的目录布局，3FS 的 diff 可以直接 apply）。
2. 第一版保持 `src/common/...` 的 include 路径和 `hf3fs::` 命名空间。改名放到最后一个可选阶段。
3. 传输格式不变（`MessageHeader`、serde 编码），因此 base-rpc 的客户端理论上仍可和 3FS 服务互通。这不是硬要求，但没有理由主动破坏。

## 1. 迁移范围

| 内容 | 处理方式 |
|---|---|
| `src/common/net/**`（含 `ib/`、`tcp/`、`sync/`） | 原样拷贝，`Server.*` 做小改（见 2.2） |
| `src/common/serde/**` | 原样拷贝，`SerdeHelper.h` 做小改（见 2.1） |
| `src/common/utils/` 中被依赖的约 44 个文件（Result/Status/StatusCode、ConfigBase、Coroutine、Address、Duration、Size、UtcTime、ObjectPool、RobinHood、ZSTD、Uuid、Path、StringUtils、SysResource、DynamicCoroutinesPool、ExecutorStatsReporter、MagicEnum/Nameof/Toml.hpp 等） | 原样拷贝 |
| `src/common/monitor/**` | **不迁移**，用空实现 shim 替代（见 2.3） |
| `src/memory/common/**` | **不迁移**，用 shim 替代（见 2.4） |
| `src/common/app/**`、`src/common/kv/**`、`src/fbs/**` | **不迁移**，切断对它们的引用 |
| `tests/common/net/**`、`tests/common/serde/**`、`tests/main`、`tests/GtestHelpers.h` | 拷贝，`TestSerde.cc` 做小改（见 3.3） |
| `cmake/{Target,CompileFlags,Sanitizers,GitVersion}.cmake`、`patches/folly.patch`、`patches/apply.sh` | 拷贝或裁剪 |

文件清单用脚本按 include 传递闭包生成：从 net 和 serde 出发，带上 `.h` 对应的 `.cc`，并排除 monitor、memory、app、kv、fbs。清单不手写，保存为 `scripts/paths.txt`。

第三方依赖（submodule，版本与 3FS 锁定一致）：folly（需打 `folly.patch`）、fmt、scnlib、toml11、zstd、googletest，系统库需要 ibverbs。**不需要** clickhouse-cpp、rocksdb、leveldb、liburing、jemalloc、mimalloc、pybind11。

## 2. 在 base-rpc 中切断的依赖

### 2.1 serde → kv
`SerdeHelper.h` 引入了 `common/kv/ITransaction.h`，只为了 `store/load/snapshotLoad` 三个 KV 辅助方法。
做法：删掉这个 include 和这三个方法（约 15 行）。

### 2.2 net::Server → common/app/AppInfo
`Server` 只用 `flat::AppInfo` 来保存信息并填充 `serviceGroups`，`getServiceGroupInfos()` 返回 `flat::ServiceGroupInfo`。
做法：
- 新增 `common/net/ServiceGroupInfo.h`，从 AppInfo.h 拷贝 `ServiceGroupInfo` 的定义（字段不变）。
- `Server.h/.cc` 删除 `appInfo_`、`appInfo()`，`start()` 去掉参数。

### 2.3 monitor → 空实现 shim
net、serde、utils 中有十几个 `.cc` 引用 `common/monitor/Recorder.h`。实际用到的 API 只有：
`CountRecorder`、`LatencyRecorder`、`ValueRecorder`、`DistributionRecorder`、`OperationRecorder`、`Recorder::TagRef`、`getRecorderWithTag`、`addSample`、`set`、`TagSet`、`threadTagSet`、`instanceTagSet`。

做法：在 base-rpc 新建 `src/common/monitor/Recorder.h` 和 `Sample.h`，类名和签名与上述 API 一致，函数体全部为空或 inline 空实现（预计不到 150 行）。调用点的代码完全不动。
`// shortcut: 指标全部丢弃；若使用方需要 metrics，再把 shim 换成对接 Prometheus 等的实现`

### 2.4 memory/common → shim
只有 `RobinHood.h` 和 `RDMABuf.cc` 用到了 `hf3fs::memory::{allocate,deallocate,memalign}`，而原实现依赖 Monitor 和 dlopen 加载的分配器。
做法：新建 `src/memory/common/GlobalMemoryAllocator.h`，用 inline 函数转发到 `std::malloc`、`std::free`、`posix_memalign`。调用点不动。

### 2.5 StatusCode
`StatusCodeDetails.h` 里混有 Meta、Storage、Mgmtd 等 3FS 业务码，而 `StatusCode.cc` 引用了它们。
- 第一版原样保留，只是数值定义，无害。
- 在 README 中为使用方划定自定义错误码的数值段，避免和已有码冲突。
- `// shortcut: 业务码暂时保留，有需要时再裁剪成 COMMON/RPC/TRANSACTION 三类`

### 2.6 version-info
`ConfigBase.cc`、`CallContext.h` 用到了 `VersionInfo`。拷贝 `cmake/GitVersion.cmake` 和 `VersionInfo.cc.in`，让它生成 base-rpc 自己的版本信息。

**依赖闸门**：`scripts/check_deps.sh` 扫描 `src/` 中所有 `#include "..."`，要求它们只指向仓库内的文件，并放进 CI。

## 3. 实施步骤

### 3.1 导出（不动 3FS 工作区）
1. `git clone /path/to/3FS /tmp/3fs-export`（全新克隆，**不在原仓库上操作**）。
2. `git filter-repo --paths-from-file scripts/paths.txt` 只保留目标文件及其历史。
3. 把结果作为初始历史合入 base-rpc：`git pull /tmp/3fs-export --allow-unrelated-histories`，记录 `UPSTREAM_COMMIT=2dcbff5`。

如果不需要历史，可以直接按清单 `rsync` 拷贝过来，一个 commit 即可。

### 3.2 让它独立编译
1. 编写顶层 `CMakeLists.txt`：
   - `project(base-rpc CXX C)`，使用 C++20，编译选项和 3FS 一致（拷贝 CompileFlags 和 Sanitizers）；
   - 选项：`BASE_RPC_BUILD_TESTS`；
   - 如果 `TARGET folly` 已存在，就复用使用方的 folly，否则 `add_subdirectory(third_party/...)`；
   - 产出静态库 `base-rpc`，对外暴露 `src/` 作为 PUBLIC include 目录。
2. 加入第三方 submodule 并打 `folly.patch`。
3. 落地 2.1–2.6 的改动和 shim，迭代到编译、链接都通过。
4. 跑依赖闸门。

### 3.3 测试
- 迁移 `tests/common/net/**`（含 `ib/`）和 `tests/common/serde/**`，以及 `tests/main`、`GtestHelpers.h`。
- `TestSerde.cc` 引用了 `fbs/mgmtd/RoutingInfo.h`：删掉相关用例，或改用测试内自定义的结构。
- 没有 RDMA 网卡的 CI 环境，依赖 `SetupIB.h` 中的 skip 逻辑跳过 IB 用例。
- 完成标准：其余用例在 TCP 环境下全绿，IB 用例在有 RDMA 网卡的机器上全绿。

### 3.4 使用方接入
- 写一个最小示例 `examples/echo`：定义 `SERDE_SERVICE` 服务，`net::Server` 启动，`net::Client` + `serde::ClientContext` 发起调用。可参考 `tests/common/net/Echo.h` 和 `TestEcho.cc`。
- README 说明：如何用 `add_subdirectory` 或 submodule 接入、配置项（`Server::Config`、`Client::Config`，基于 toml 的 ConfigBase）、错误码约定、IB 和 TCP 的区别。
- `// shortcut: 暂不提供 install/find_package，有非 CMake 子项目接入需求时再加`

## 4. 与 3FS 上游同步

- 因为目录布局相同，同步方式如下：
  ```
  git -C 3FS diff $UPSTREAM_COMMIT..$NEW -- $(cat scripts/paths.txt) | git apply -3
  ```
  冲突只会出现在少数改过的文件上（SerdeHelper.h、Server.*，以及 shim 替代掉的那些头文件对应的上游改动）。
- 脚本化为 `scripts/sync_upstream.sh`，并更新 `UPSTREAM_COMMIT`。
- 上游新增 include 依赖时，由依赖闸门报警，再决定是拷贝还是 shim 掉。

## 5. 可选的后续阶段

- 改名：命名空间 `hf3fs::` 改为 `brpc::` 一类的名字，include 前缀 `common/` 改为 `base_rpc/`，避免使用方的 `common/utils/...` 路径冲突。代价是此后上游同步只能手工 cherry-pick。**建议等上游同步需求降低后再做。**
- 裁剪 StatusCode 中的业务码。
- 提供纯 TCP 构建（`BASE_RPC_ENABLE_IB=OFF`）：`IBSocket`、`RDMABuf`、`RDMAControl` 在 Transport、CallContext 中有引用，需要加编译开关，工作量要单独评估。
- 真正可用的 metrics 实现（替换 2.3 的 shim）。
- `install()` 和 `base-rpcConfig.cmake`。

## 6. 风险

| 风险 | 缓解 |
|---|---|
| shim 签名和调用点对不上 | 以编译为准，按报错补齐；shim 只做最小 API |
| 闭包里还有隐性依赖（如 `.cc` 里的 include 或符号） | 依赖闸门脚本 + 链接全部测试二进制 |
| folly 版本或补丁与使用方已有的 folly 冲突 | 锁定 3FS 的 folly 版本；使用方已有 folly 时复用其 target，并在 README 写明版本要求 |
| 默认依赖 ibverbs，部署门槛高 | 第一版要求安装 rdma-core 开发包；纯 TCP 构建放在后续阶段 |
| 与 3FS 分叉后难以同步 | 保持布局不变，少改原文件，同步脚本化 |

## 7. 工作量估算

| 步骤 | 估时 |
|---|---|
| 清单脚本 + 导出 | 0.5 天 |
| CMake 骨架 + 第三方库接入 | 1–1.5 天 |
| 2.1–2.6 依赖切断与 shim，编译通过 | 1.5–2 天 |
| 测试迁移，跑通 TCP 与 IB 测试 | 1 天 |
| echo 示例、README、同步脚本 | 1 天 |
| **合计** | **约 5–6 人天** |
