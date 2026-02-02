# RoceSender

单进程 RDMA 收发包示例，使用 MPI 在两台设备上跑单进程（每台一进程）进行 RDMA ping-pong 测试，适用于 RoCE 网络。

## 依赖

- MPI（如 OpenMPI、MPICH）
- libibverbs（RDMA verbs 库）
- 支持 RoCE 的网卡与已配置的 RDMA 驱动

## 构建

```bash
make
```

## 运行

```bash
mpirun -np 2 -H <host1>,<host2> ./rdma_pingpong -d <ib_dev> -p 1 -s 64 -n 10000
```

参数说明：

- `-d/--device`：IB 设备名（如 `mlx5_0`，默认选择第一个设备）
- `-p/--port`：IB 端口（默认 1）
- `-s/--size`：消息大小（字节，默认 64）
- `-n/--iters`：迭代次数（默认 10000）
- `-l/--sl`：Service Level（默认 0）

## 说明

- 程序使用 RC QP 进行 SEND/RECV 交互，通过 MPI 交换 QP 信息。
- rank 0 统计 RTT 与单向时延（RTT/2）。
- 适用于 loopback/直连等 RoCE 环境测试。
