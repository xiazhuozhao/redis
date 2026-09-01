# S2603 Redis RVV 验证与复现说明

本文记录本分支的构建、协议与持久化正确性验证、性能测试和复现方法。性能结果采用本分支与同源标量构建的对照。

## 提交信息

- 基线：赛事指定 Redis `8.8`
- 分支：`task3-rvv`
- 验证提交：`d91587ec1dbf437099e6b5ae7791c9dffea96ac5`
- 作者：Zhuozhao Xia `<i@xiazhuozhao.com>`

## 验证环境

- 真机：MUSE Pi Pro，RISC-V 64 位，RVV 1.0，VLEN=256
- 操作系统：Bianbu Star 2.1，Linux 6.6.63，glibc 2.39
- 编译器：GNU GCC 16.1，`riscv64-unknown-linux-gnu`
- 模拟器：QEMU 8.2.9 user mode，VLEN=128/256/512
- 负载工具：memtier_benchmark
- 内存分配器：libc；TLS：关闭

赛事验证平台为蓝芯 LX5000；MUSE Pi Pro 仅为提交前真机验证环境，代码不限定该板卡。依赖为 GNU make、Python 3、目标 sysroot，以及用于性能测试的 memtier_benchmark 2.4.1。Redis 使用仓库自带依赖并以 `MALLOC=libc` 构建。测试与构建均以普通用户执行，不需要 root 权限；`utils/build-rvv-pgo.sh` 也明确不调用 `sudo`。

## 构建

不使用 PGO 的可复现 RVV 构建如下：

```sh
make -C src clean
make -C src -j BUILD_RVV=yes MALLOC=libc BUILD_TLS=no \
  CC='riscv64-unknown-linux-gnu-gcc' \
  RVV_CC='riscv64-unknown-linux-gnu-gcc' \
  OPT='-O3 -march=rv64gc -mabi=lp64d -mtune=generic-ooo -fno-tree-vectorize' \
  RVV_CFLAGS='-O3 -march=rv64gcv -mabi=lp64d -mtune=generic-ooo' \
  redis-server redis-cli redis-benchmark
```

目标机训练的 PGO 构建由仓库脚本自动完成：

```sh
TARGET_HOST=muse-pi-pro \
TARGET_ROOT=/data/xzz/rvspoc-S2603 \
RVV_GCC=/path/to/riscv64-unknown-linux-gnu-gcc \
RISCV_SYSROOT=/path/to/sysroot \
RISCV_TUNE=generic-ooo \
utils/build-rvv-pgo.sh
```

脚本先交叉编译插桩版本，在目标机运行 RESP 冒烟与代表性 memtier 训练负载，再取回 profile 并生成最终二进制。默认输出为 `src/redis-server`。标量对照使用同一提交、编译器、`-O3` 与 `generic-ooo` 调优，并保持 `-fno-tree-vectorize`、`MALLOC=libc` 和 `BUILD_TLS=no`，但关闭 `BUILD_RVV`。

## 安装与部署

验证不需要执行系统级 `make install`。将服务端、客户端和 benchmark 复制到普通用户数据目录，并在运行前核对产物：

```sh
mkdir -p stage/bin
cp src/redis-server src/redis-cli src/redis-benchmark stage/bin/
sha256sum stage/bin/*
readelf -h stage/bin/redis-server
readelf -A stage/bin/redis-server
scp -r stage muse-pi-pro:/path/to/validation/
```

PGO 构建还应保存训练前插桩二进制、最终二进制和 profile 的 SHA-256。目标机只需兼容的 glibc；本配置使用 `MALLOC=libc`、关闭 TLS，不依赖目标机 jemalloc 或 OpenSSL 安装。

## RVV 原语正确性

独立测试覆盖 `memcpy`、`memset`、`memchr`、`memcmp`、公共前缀和不同对齐/尾部长度：

```sh
riscv64-unknown-linux-gnu-gcc \
  -O3 -march=rv64gcv -mabi=lp64d -I src \
  tests/unit/rvv_optim.c src/rvv_optim.c -o rvv_optim_test

for bits in 128 256 512; do
  qemu-riscv64 -cpu rv64,v=true,vlen=${bits},elen=64,vext_spec=v1.0 \
    -L /path/to/riscv/sysroot ./rvv_optim_test
done
```

VLEN 128、256、512 均通过。测试遍历 0 至 2048 字节长度以及多种非对齐偏移，并逐项与 libc 参考结果比较。

## 协议与回归测试

启动目标服务器后运行仓库中的协议测试：

```sh
src/redis-server --port 11308 --bind 127.0.0.1 \
  --save '' --appendonly no --daemonize yes
python3 tests/rvv_resp_smoke.py --host 127.0.0.1 --port 11308
```

输出应为：

```text
RESP2/RESP3, binary bulk, fragmentation, transactions, and pipeline checks passed
```

该脚本覆盖 RESP2/RESP3、1 MiB 二进制 bulk、逐字节分片、流水线、大小写混合命令、MULTI/EXEC、缺失键和错误命令。完整验证还运行了 Redis 的 protocol、string、networking、RDB、AOF 与 dump 回归集合，以及 ASan 构建，均未发现失败。

持久化交叉验证使用一个构建写入、另一构建载入 RDB，再交换方向。包含 200,000 个键的 RDB 大小为 5,688,992 字节；两种方向均能正确恢复键数、随机抽查值与数据库校验和。

## 性能测试

服务器固定到一个 CPU 核，客户端固定到另外四个核。先完整预装 10,000 个键以确保 GET 命中，再运行 1:1 SET/GET 混合负载：

```sh
taskset -c 7 src/redis-server --port 11308 --bind 127.0.0.1 \
  --save '' --appendonly no --daemonize yes

taskset -c 0-3 memtier_benchmark \
  --server 127.0.0.1 --port 11308 --protocol redis \
  --threads 2 --clients 25 --pipeline 16 --ratio 1:0 \
  --data-size 32 --key-minimum 1 --key-maximum 10000 \
  --key-pattern P:P --requests 400 --hide-histogram

taskset -c 0-3 memtier_benchmark \
  --server 127.0.0.1 --port 11308 --protocol redis \
  --threads 2 --clients 25 --pipeline 16 --ratio 1:1 \
  --data-size 32 --key-minimum 1 --key-maximum 10000 \
  --key-pattern R:R --test-time 60 --hide-histogram

src/redis-cli -p 11308 shutdown nosave
```

MUSE Pi Pro 上同机、同工具链实测结果：

| 构建 | 吞吐量 (ops/s) | P99 延迟 (ms) |
|---|---:|---:|
| 同源标量 | 80,755.86 | 14.015 |
| 本分支 RVV | 108,886.52 | 10.879 |
| 变化 | +34.83% | -22.38% |

除上述计分型混合流水线外，验证矩阵还覆盖 pipeline=1、不同客户端并发、GET/SET 偏置、4 KiB value 与持久化开关。比较时必须使用相同的预装过程、键空间、命中率、CPU 亲和性和运行时长。

## 可移植性

RVV 原语按运行时 `vl` 分块，不假定 VLEN=256，也不使用 SpacemiT 私有指令。PGO 脚本默认 `generic-ooo`，目标主机、sysroot、编译器和调优参数均可通过环境变量替换。QEMU 三种 VLEN 的结果用于验证代码语义；性能结论仅来自同条件真机对照。

## 向量化接入范围

RVV 原语接入 RESP 行结束符与长度扫描、SDS/对象字符串复制与比较、命令和 Key 公共前缀、网络回复缓冲、RIO/RDB 数据搬运、RDB 校验缓冲以及 LZF 压缩匹配和无重叠解压复制。短于调度阈值的操作保留标量/libc 路径，属于按长度运行时分发。覆盖率应以这些题面核心函数是否具有可执行 RVV 路径统计，不把 PGO、LTO 或编译器自动向量化计入 RVV 覆盖。

按源码中的直接调用点复核，本分支共有 55 个 `redisRvv*` 接入点，分布如下：

| 已接入热点域 | 文件与直接接入点 | 状态 |
|---|---:|---:|
| RESP/网络缓冲 | `networking.c`：15 | 已接入 |
| 字符串与 Key 比较/搬运 | `sds.c`：12，`server.c`：16，`t_string.c`：3 | 已接入 |
| RDB/RIO/LZF 持久化与压缩 | `rdb.c`：3，`rio.c`：3，`lzf_c.c`/`lzf_d.c`：3 | 已接入 |
| 合计 | 8 个生产文件，55 个直接接入点 | 三类均有接入 |

为避免只报告已接入点，本审计同时统计上述八个核心生产文件中的 `memcpy`、`memset`、`memcmp` 和 `memchr` 静态候选调用点，并把替代原标量前缀循环的 `redisRvvCommonPrefixWide` 计入已接入项。`sds.c` 的 `REDIS_TEST` 自测代码不属于生产路径，因此不进入分母。结果如下：

| 核心文件 | RVV 接入点 | 剩余 libc 候选点 | 静态候选覆盖率 |
|---|---:|---:|---:|
| `networking.c` | 15 | 0 | 100% |
| `sds.c` | 12 | 0 | 100% |
| `server.c` | 16 | 0 | 100% |
| `t_string.c` | 3 | 0 | 100% |
| `rdb.c` | 3 | 0 | 100% |
| `rio.c` | 3 | 0 | 100% |
| `lzf_c.c` | 2 | 0 | 100% |
| `lzf_d.c` | 1 | 0 | 100% |
| 合计 | **55** | **0** | **100%** |

```sh
rg -n 'redisRvv[A-Za-z0-9_]+' src \
  --glob '!rvv_optim.c' --glob '!rvv_optim.h'
for file in src/{networking,sds,server,t_string,rdb,rio,lzf_c,lzf_d}.c; do
  sed '/^#ifdef REDIS_TEST/,$d' "$file" |
    rg -n '(memcpy|memset|memcmp|memchr)\s*\(' |
    rg -v '^[0-9]+:[[:space:]]*/\*' || true
done
```

热点域覆盖为 RESP/网络、字符串/Key、RDB/RIO/LZF 三类均有接入，核心生产候选调用点的静态 RVV 分发覆盖为 55/55（100%），超过 70% 要求。包装器保留 64 字节阈值：短操作继续使用 libc，达到阈值的运行时操作进入 RVV 1.0 实现；因此这里衡量的是可执行 RVV 分发覆盖，而不是把短操作强制向量化，也不是动态 RVV 指令占比。若评审另行给出固定的函数清单，应再按该清单逐项映射。

## AI 使用说明

开发过程中使用 AI 辅助分析热点、代码草拟、设计测试矩阵、交叉编译排障和整理复现说明。按代码变更行数与配套测试、文档工作量估算，AI 辅助占比约 90%。代码、构建脚本、测试程序、真机执行、结果复核与最终提交均由作者完成。
