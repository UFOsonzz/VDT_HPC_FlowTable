# FlowTable DPDK

FlowTable là mini project data plane IPv4 dùng DPDK để nhận packet từ NIC hoặc
PCAP PMD, chuẩn hóa flow hai chiều, phân tải flow theo worker, áp dụng SPI rule
và xuất thống kê runtime. Repo này được triển khai theo yêu cầu trong
`MiniProject_HPC_Flowtable.docx` và rule workbook `SPI_DPI_rule.xlsx`.

## Kiến Trúc

Đường xử lý chính là pipeline chạy song song theo core:

1. DPDK ethdev hoặc PCAP PMD nhận packet theo burst.
2. Dispatcher parse Ethernet, VLAN tùy chọn, IPv4, TCP/UDP.
3. Direction resolver xác định tenant và uplink/downlink từ hint, ingress port,
   VLAN hoặc subnet.
4. Packet được chuẩn hóa thành canonical five-tuple để hai chiều của cùng flow
   có cùng key.
5. Dispatcher chọn worker bằng hash. Fixed-worker mode dùng hash trực tiếp;
   dynamic mode dùng owner-map để hỗ trợ `scale up/down`.
6. Worker sở hữu riêng flow table shard, SPI action cache, aging và counters.
7. CLI/dashboard đọc counters để hiển thị active flow, throughput, drop, worker
   detail, traffic class và rule hit.

Multi-dispatcher/multi-RX chỉ bật trong fixed-worker mode. Khi dùng nhiều
dispatcher, mỗi dispatcher có SPSC ring riêng tới từng worker để tránh shared
enqueue lock.

## Cấu Trúc Hiện Tại

```text
.
├── Makefile
├── MiniProject_HPC_Flowtable.docx
├── SPI_DPI_rule.xlsx
├── config/
│   ├── direction_rules.csv
│   └── spi_rules.csv
├── docs/
│   ├── report.tex
│   ├── report.pdf
│   ├── dashboard_realtime.png
│   ├── worker_realtime.png
│   └── beamer/theme assets
├── include/
│   ├── ft_common.h
│   ├── ft_config.h
│   ├── ft_flow.h
│   ├── ft_packet.h
│   ├── ft_pipeline.h
│   ├── ft_rule.h
│   └── ft_stats.h
├── reports/
│   ├── BENCHMARK.md
│   └── e2e_benchmark_results.csv
├── scripts/
│   ├── generate_spi_pcap.py
│   ├── generate_test_workbook.py
│   ├── run_cli_pcap.sh
│   ├── run_e2e_benchmark.sh
│   ├── run_e2e_benchmark_huge.sh
│   └── setup_hugepages_if_needed.sh
├── src/
│   ├── config.c
│   ├── control.c
│   ├── dispatcher.c
│   ├── flow.c
│   ├── main.c
│   ├── packet.c
│   ├── pipeline.c
│   ├── pipeline_internal.h
│   ├── port.c
│   ├── rule.c
│   ├── stats.c
│   └── worker.c
└── tests/
    ├── FlowTable_Test_Cases.xlsx
    ├── data/spi_rules_test.csv
    └── test_flowtable.c
```

`generated/` chứa PCAP sinh tự động cho benchmark và có thể tạo lại từ script.
`build/` là output build local.

## Build Và Test

Yêu cầu môi trường:

- Linux
- GCC hoặc Clang
- GNU Make
- DPDK có `pkg-config` package `libdpdk`
- Python 3 cho các script sinh PCAP/workbook

Build chương trình và unit test:

```bash
make -j
make test
```

`make test` chạy test binary qua DPDK EAL với `--no-huge --in-memory --no-pci`,
phù hợp để kiểm tra logic parser, flow table, rule, direction và worker
distribution mà không cần setup hugepages.

## Hugepages

Benchmark E2E không dùng `--no-huge`, nên cần chuẩn bị hugepages trước:

```bash
sudo scripts/setup_hugepages_if_needed.sh --mb 1024 --mount-point /mnt/huge
```

Script benchmark mặc định kiểm tra hugepages bằng `--check-only`. Nếu hugepages
chưa sẵn sàng, benchmark sẽ dừng và in lệnh setup cần chạy.

Có thể đổi mount point hoặc tổng dung lượng bằng biến môi trường:

```bash
HUGEPAGE_MB=1024 HUGEPAGE_MOUNT=/mnt/huge make benchmark-e2e
```

## Benchmark E2E

Chạy benchmark chính:

```bash
make benchmark-e2e
```

Benchmark được điều khiển bởi `scripts/run_e2e_benchmark.sh`:

- sinh PCAP từ `SPI_DPI_rule.xlsx` bằng `scripts/generate_spi_pcap.py`;
- dùng PCAP PMD với `infinite_rx=1`;
- PCAP gốc mặc định có `100000` flow và `200000` packet;
- app xử lý mặc định `BENCH_PACKETS=2000000` packet nhờ replay PCAP;
- chạy `2` warmup runs, bỏ qua kết quả warmup;
- chạy `5` measured runs và lấy median PPS;
- ghi kết quả vào `reports/e2e_benchmark_results.csv`.

Hai profile mặc định:

| Profile | Topology | Packet xử lý | Kết quả hiện tại |
|---|---|---:|---:|
| `pcap-spi-4w-huge` | 1 RX queue, 1 dispatcher, 4 workers | 2,000,000 | 6.75 Mpps |
| `pcap-spi-mq-2d4w-huge` | 2 RX queues, 2 dispatchers, 4 workers | 2,000,000 | 10.26 Mpps |

PPS là thông lượng packet của toàn pipeline, không phải trung bình mỗi worker.
Kết quả chi tiết nằm trong `reports/BENCHMARK.md` và
`reports/e2e_benchmark_results.csv`.

Một vài biến môi trường hữu ích:

```bash
E2E_WARMUP_RUNS=0 E2E_RUNS=1 make benchmark-e2e
PCAP_FLOWS=100000 PCAP_PACKETS=200000 BENCH_PACKETS=2000000 make benchmark-e2e
MQ_DISPATCHERS=2 MQ_WORKERS=4 make benchmark-e2e
```

Nếu tắt `PCAP_INFINITE_RX`, packet limit không được lớn hơn số packet trong PCAP:

```bash
PCAP_INFINITE_RX=0 PCAP_PACKETS=200000 BENCH_PACKETS=200000 make benchmark-e2e
```

## Runtime CLI

CLI dùng để quan sát và điều khiển runtime, không phải benchmark chính thức.
Lệnh nhanh:

```bash
scripts/run_cli_pcap.sh
```

Script này tự build nếu thiếu binary, kiểm tra hugepages, sinh PCAP nếu thiếu và
mở `./build/flowtable` với PCAP PMD. Mặc định:

```text
WORKERS=2
MAX_WORKERS=4
PACKETS=0
PCAP_INFINITE_RX=1
FIXED_WORKERS=0
```

Nghĩa là CLI mặc định chạy dynamic mode `2/4` worker để test `scale up/down`.
Các lệnh trong CLI:

```text
help
show statistics
show flow
show worker
show worker N
show traffic
show dashboard
rules
reload
scale up
scale down
quit
```

Nếu muốn CLI chạy gần giống benchmark fixed-worker 4 worker:

```bash
FIXED_WORKERS=1 WORKERS=4 MAX_WORKERS=4 scripts/run_cli_pcap.sh
```

Khi xem dashboard, `pps` là tốc độ theo interval gần nhất, còn `avg pps` là
trung bình từ lúc start. Hai số này không tương đương median PPS trong
benchmark E2E.

## Chạy Trực Tiếp Với PCAP PMD

Ví dụ replay một PCAP Ethernet:

```bash
sudo ./build/flowtable \
  -l 0-4 --huge-dir /mnt/huge --in-memory --no-pci \
  --vdev 'net_pcap0,rx_pcap=traffic_vlan.pcap,infinite_rx=1' --no-telemetry -- \
  --mode ethdev --port 0 --workers 4 --max-workers 4 \
  --packets 0 --rx-mbufs 208192 --cli
```

`--packets 0` nghĩa là chạy đến khi nhận `SIGINT` hoặc `quit` trong CLI.

Ví dụ multi-RX/multi-dispatcher fixed-worker:

```bash
sudo ./build/flowtable \
  -l 0-6 --huge-dir /mnt/huge --in-memory --no-pci \
  --vdev 'net_pcap0,rx_pcap=generated/spi_benchmark_mq_q0.pcap,rx_pcap=generated/spi_benchmark_mq_q1.pcap,infinite_rx=1' \
  --no-telemetry -- \
  --mode ethdev --port 0 --workers 4 --max-workers 4 \
  --packets 2000000 --rx-mbufs 208192 \
  --rx-queues 2 --dispatchers 2 --per-dispatcher-limit --fixed-workers
```

## Rule Và Direction

`config/spi_rules.csv` là SPI rule CSV được chuyển từ workbook. Rule được match
khi flow mới được tạo; action và rule id được cache trong flow entry để packet
sau của cùng flow không phải scan lại ruleset.

`config/direction_rules.csv` định nghĩa cách xác định tenant và hướng:

```text
match_type,value,tenant_id,direction
SRC_PREFIX,10.0.0.0/8,1,UPLINK
DST_PREFIX,10.0.0.0/8,1,DOWNLINK
VLAN,100,1,UPLINK
INGRESS_PORT,1,1,DOWNLINK
```

Thứ tự resolver:

```text
explicit packet hint -> ingress port -> VLAN -> source/destination prefix
```

Nếu không có rule direction nào match, packet vẫn được tạo symmetric key bằng
thứ tự endpoint nhưng direction là `UNKNOWN`.

## Module Chính

| File | Vai trò |
|---|---|
| `src/main.c` | Parse EAL/app options và khởi động pipeline. |
| `src/config.c` | Validate/default runtime config. |
| `src/port.c` | Setup ethdev, queue, mempool, RX/TX. |
| `src/packet.c` | Parser Ethernet/VLAN/IPv4/TCP/UDP, direction resolver, canonical key. |
| `src/dispatcher.c` | RX burst, chọn worker, owner-map dynamic, ring enqueue. |
| `src/worker.c` | Worker loop, flow lookup/create, SPI cache, action, aging, counters. |
| `src/flow.c` | Per-worker flow table, lifecycle, timeout, migration helper. |
| `src/rule.c` | SPI CSV loader, precedence, wildcard match, rule hit. |
| `src/control.c` | Signal, CLI command, reload, scale up/down. |
| `src/stats.c` | CLI tables, dashboard ANSI, graphs và summary counters. |
| `src/pipeline.c` | Orchestration, lcore launch, dispatcher/worker lifecycle. |

## Tài Liệu Và Kết Quả

- `docs/report.tex`: báo cáo LaTeX.
- `docs/report.pdf`: PDF report hiện tại.
- `docs/dashboard_realtime.png`, `docs/worker_realtime.png`: ảnh minh họa CLI.
- `reports/BENCHMARK.md`: tóm tắt test và benchmark.
- `reports/e2e_benchmark_results.csv`: CSV kết quả benchmark mới nhất.
- `tests/FlowTable_Test_Cases.xlsx`: workbook test case gồm functional và
  performance scenarios.

## Trạng Thái Xác Minh Hiện Tại

- Unit test: `127/127` passed.
- PCAP PMD smoke path: processed packet, active flow và drop counters reconcile.
- E2E benchmark đã đo với hugepages, warmup, median run và `infinite_rx=1`.
- Chưa đo: NIC line-rate vật lý, packet loss trên NIC thật, latency p50/p99,
  OS-sampled CPU utilization và aging pressure benchmark dài hơn timeout.
