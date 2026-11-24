# base-rpc

Network and RPC library extracted from [3FS](https://github.com/deepseek-ai/3FS) (MIT, see `LICENSE`):
`common/net` (TCP / RDMA / unix transports, event loop, server, client) and `common/serde`
(serialization and the `SERDE_SERVICE` RPC framework), plus the few `common/utils` pieces they need.

Exported from 3FS commit `UPSTREAM_COMMIT`. Include paths (`common/...`) and the `hf3fs::` namespace are unchanged,
so the wire protocol is the same as 3FS and 3FS fixes can be re-applied with `scripts/sync_upstream.sh`.

## Build

Requirements: **gcc 13+ or clang** (tested: gcc 13 full build + tests, gcc 14 one TU, clang 21; gcc 15 hits an internal compiler error in `CallContext.h`), CMake >= 3.12,
Boost (filesystem, system, program_options, plus whatever folly needs), glog, gflags, double-conversion, libevent,
OpenSSL, and rdma-core headers/library (`libibverbs`, needed even for TCP-only use in this version).
folly, fmt, scnlib, toml11, zstd and googletest are git submodules (`git submodule update --init`); folly gets
`patches/folly.patch` applied at configure time.

```sh
cmake -S . -B build -G Ninja            # add -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang for clang
cmake --build build
ctest --test-dir build
./build/examples/echo
```

Notes:
- gcc compiles slowly: every TU that includes the net/serde headers takes 30s+ (clang: a few seconds).
- (clang) pinned fmt (10.0.0-71) breaks with clang >= 19 consteval: add `-DCMAKE_CXX_FLAGS=-DFMT_CONSTEVAL=`.
- (clang) if clang picks a too new libstdc++ (gcc 15 fails in folly `OrderedMap`), add `--gcc-install-dir=<gcc 13 dir>`.
- if a system fmt/folly sits in the include path before `third_party/fmt`, point `CMAKE_PREFIX_PATH` at a prefix without them.

## Use from another project

```cmake
add_subdirectory(third_party/base-rpc)       # or FetchContent
target_link_libraries(my_app PRIVATE base-rpc)
```

- A parent project may provide its own `folly`, `fmt`, `scn::scn`, `toml11`, `libzstd_static` targets; base-rpc then
  skips its submodule copy. The folly must contain the 3FS patches.
- Build everything with `-msse4.2 -mavx2` on x86_64 (folly F14 containers depend on it; base-rpc sets it only when it
  is the top-level project).
- See `examples/echo.cc` for a server + client. Service ids and error codes: base-rpc keeps 3FS's
  `StatusCodeDetails.h`, so pick new codes that do not collide with the existing ones.
- TCP and unix sockets work without RDMA hardware; `Address::RDMA` needs an RDMA NIC.

## Differences from 3FS

Copied files are unchanged except the following. The gcc workarounds sit behind `#ifdef __clang__`, so clang compiles the original 3FS code:

| File | Change |
|---|---|
| `common/serde/Serde.h`, `common/net/ib/RDMABuf.h` | `#ifdef __clang__` keeps 3FS's `struct ::ns::SerdeMethod<...>` heads; other compilers get them without the leading `::` (gcc rejects it) |
| `common/serde/Serde.cc` | `Out<JsonObject/TomlObject>::add` definition: clang keeps the original, gcc gets `template <> template <class U> ... add(U &&v)` (it rejects the abbreviated form) |
| `common/serde/Services.h` | clang keeps the `std::array`; other compilers use a `std::vector` for `services_` (gcc unrolled the array construction into a 5MB constructor, compile took minutes) |
| `common/serde/SerdeHelper.h` | removed `store/load/snapshotLoad` (KV transaction helpers) |
| `common/net/Server.{h,cc}` | no `AppInfo`: `start()` takes no argument, no `appInfo()` |
| `common/net/ServiceGroupInfo.{h,cc}` | new, `flat::ServiceGroupInfo` copied from `common/app/AppInfo.h` |
| `common/monitor/{Recorder,Sample}.h` | **shim**: all metrics are no-ops |
| `memory/common/GlobalMemoryAllocator.h` | **shim**: malloc/free/posix_memalign |
| `compat/gtest/gtest_prod.h` | `FRIEND_TEST` without needing gtest installed |
| `tests/common/serde/TestSerde.cc` | gcc only: parentheses around `a > 0` in a lambda inside a macro (gcc parse bug) |
| `tests/main/TestMain.cc` | simplified (no 3FS log handlers / monitor) |
| `tests/common/net/ib/SetupIB.h`, `TestEcho.cc`, `TestProcessor.cc`, `TestRDMAControl.cc`, `TestService.cc` | RDMA tests skip when there is no RDMA device |

Not carried over: monitor, `common/app`, `common/kv`, `fbs`, `GitVersion` / version-info (`VersionInfo.h` is
only included, never called).

## Scripts

- `scripts/gen_paths.py <3FS>`: recompute the exported file list (`scripts/paths.txt`).
- `scripts/export.sh <3FS>`: initial copy (overwrites the modified files above, do not rerun after the first export).
- `scripts/sync_upstream.sh <3FS> [commit]`: apply upstream changes to the exported files.
- `scripts/check_deps.py`: fails if an include points outside this repo.

## RDMA tests

`tests/common/net/ib` needs a real RDMA NIC: configure with `-DBASE_RPC_RUN_IB_TESTS=ON` to register it in ctest.
