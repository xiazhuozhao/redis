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

## AI 使用说明

开发过程中使用 AI 辅助分析热点、代码草拟、设计测试矩阵、交叉编译排障和整理复现说明。按代码变更行数与配套测试、文档工作量估算，AI 辅助占比约 90%。代码、构建脚本、测试程序、真机执行、结果复核与最终提交均由作者完成。
