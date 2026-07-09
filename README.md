# FlowTable DPDK

FlowTable là data plane IPv4 hiệu năng cao dùng DPDK, được xây dựng từ
`MiniProject_HPC_Flowtable.docx` và sheet `SPI_rule` trong
`SPI_DPI_rule.xlsx`.

Thiết kế dùng pipeline và ownership theo core:

1. RX từ NIC hoặc PCAP PMD nhận packet theo burst.
2. Parser lấy Ethernet, VLAN tùy chọn, IPv4, TCP/UDP.
3. Direction Resolver xác định tenant và uplink/downlink từ metadata, ingress
   port, VLAN hoặc IP subnet.
4. Five-tuple được chuẩn hóa thành khóa client/server hai chiều.
5. Symmetric hash đưa cả hai chiều vào cùng worker. Với multi-dispatcher, mỗi
   dispatcher có một SPSC ring riêng tới từng worker để tránh shared enqueue
   lock.
6. Worker sở hữu riêng một shard `rte_hash`, flow state, rule cache, aging và
   counters; không có lock trên fast path.
7. SPI chỉ match khi tạo flow. Rule/action được lưu trong flow entry và tái sử
   dụng cho mọi packet uplink/downlink.

## Tài liệu

- [SRS](docs/01_SRS.md)
- [High-level design](docs/02_HIGH_LEVEL_DESIGN.md)
- [Flow diagrams](docs/03_FLOW_DIAGRAMS.md)
- [Software detailed design](docs/04_SOFTWARE_DESIGN.md)
- [Kiến trúc, code guide và hướng cải tiến](docs/05_ARCHITECTURE_CODE_GUIDE.md)
- [Kết quả kiểm thử và benchmark](reports/BENCHMARK.md)
- [Đối chiếu yêu cầu docx](reports/PROJECT_REQUIREMENT_REVIEW.md)
- [Design/data-flow/sequence/activity diagrams](reports/DESIGN_DIAGRAMS.md)
- [Test cases Excel](tests/FlowTable_Test_Cases.xlsx) gồm đúng hai sheet:
  `Functional Test` và `Performance Test`

## Yêu cầu build

- Linux, GCC/Clang, GNU Make
- DPDK có `pkg-config` package `libdpdk`
- Production: hugepages và NIC/PMD phù hợp

Build và test:

```bash
make -j
make test
sudo scripts/setup_hugepages_if_needed.sh --mb 1024 --mount-point /mnt/huge
make benchmark-e2e
```

`make benchmark-e2e` mặc định kiểm tra hugepages trước khi chạy, bỏ
`--no-huge` và truyền `--huge-dir /mnt/huge` cho DPDK. Benchmark vẫn dùng
`--in-memory` để tránh DPDK multiprocess socket trên laptop/sandbox; cờ này
không bật chế độ no-huge. Có thể đổi bằng `HUGEPAGE_MB` và `HUGEPAGE_MOUNT`.

E2E benchmark mặc định chạy 2 warmup runs, 5 measured runs và ghi median run
vào `reports/e2e_benchmark_results.csv`. PCAP benchmark mặc định được sinh lại
từ `SPI_DPI_rule.xlsx` bằng `scripts/generate_spi_pcap.py`, với
`PCAP_FLOWS=100000`, `PCAP_PACKETS=200000` và `PCAP_INFINITE_RX=1` trên PCAP
PMD. Tỉ lệ này tạo khoảng hai packet cho mỗi flow, phù hợp kiểu benchmark
100k flow thường dùng. Có thể chạy nhanh hơn khi dev bằng
`E2E_WARMUP_RUNS=0 E2E_RUNS=1 make benchmark-e2e`. Các profile fixed-worker
dùng `--fixed-workers` để tắt runtime scaling và dispatch flow trực tiếp bằng
hash canonical key.
Khi bật `infinite_rx=1`, script tự truyền `--rx-mbufs` đủ lớn cho PCAP PMD;
có thể override bằng `PCAP_RX_MBUFS` hoặc `MQ_RX_MBUFS`.
Profile multi-RX mặc định dùng `MQ_DISPATCHERS=2`, sinh các PCAP shard riêng
và truyền nhiều `rx_pcap` vào PCAP PMD. Profile này bật
`--per-dispatcher-limit` để mỗi dispatcher xử lý đúng shard của nó khi
`infinite_rx=1`, giữ cardinality ở 100k flow. Với dataset mặc định, script tự
sinh lại shard để tránh dùng nhầm PCAP cũ không đủ packet; nếu tự truyền
`MQ_PCAP_PREFIX`, đặt `MQ_REGENERATE=1` khi đổi số shard hoặc kích thước.

## Chạy với PCAP PMD

DPDK PCAP PMD xuất hiện như một ethdev. Ví dụ:

```bash
sudo ./build/flowtable \
  -l 0-4 --vdev 'net_pcap0,rx_pcap=traffic.pcap' -- \
  --mode ethdev --port 0 --workers 4 --packets 0
```

`--packets 0` chạy đến khi nhận `SIGINT`/`SIGTERM`. Không thêm `--tx` nếu chỉ
muốn replay, phân loại và giải phóng packet. `--tx` bật một TX queue riêng cho
mỗi worker và cần PMD/port hỗ trợ đủ queue.

Multi-dispatcher/multi-RX chỉ bật trong fixed-worker mode. Ví dụ hai RX queue,
hai dispatcher và bốn worker:

```bash
sudo ./build/flowtable \
  -l 0-6 \
  --vdev 'net_pcap0,rx_pcap=generated/spi_benchmark_mq_q0.pcap,rx_pcap=generated/spi_benchmark_mq_q1.pcap' -- \
  --mode ethdev --port 0 --workers 4 --max-workers 4 --packets 200000 \
  --rx-queues 2 --dispatchers 2 --fixed-workers
```

Dynamic scaling vẫn dùng single dispatcher/owner-map để tránh concurrent
ownership update trong dispatcher.

## Runtime control

`--workers` là số worker active ban đầu, còn `--max-workers` là số worker được
launch sẵn để scale-up runtime. Dispatcher giữ owner-map riêng nên flow đã có
owner không bị đổi worker khi active worker count thay đổi.

- `SIGUSR1`: tăng active worker count thêm một, tối đa `--max-workers`
- `SIGUSR2`: giảm active worker count thêm một, tối thiểu một worker
- `SIGHUP`: reload file `--rules` cho flow mới; flow cũ giữ action đã cache
- `--stats-interval N`: in live stats mỗi `N` giây
- `--dashboard`: đổi output interval thành dashboard realtime ANSI; nếu chưa
  truyền `--stats-interval` thì interval mặc định là 1 giây. Dashboard có
  graph ASCII cho Active Flow, Throughput và Packet Drop.
- `--cli`: bật CLI terminal trong ethdev mode với `show statistics`,
  `show benchmark`, `show flow`, `show worker`, `show worker N`,
  `show traffic`, `show dashboard`, `rules`, `reload`, `scale up`,
  `scale down`, `quit`.
  `show benchmark` hiển thị elapsed time, interval/average PPS,
  flow-create-rate và packet drops theo thời gian thực. Khi PCAP PMD bật
  `infinite_rx=1`, lệnh này dùng được như một màn hình benchmark live.
  `show worker N` hiển thị riêng một worker core, gồm queue/lcore/socket,
  packet/byte/drop, flow lifecycle, sáu traffic classes
  `HTTP/HTTPS/DNS/TCP/UDP/OTHER` và direction counters.

Ví dụ bật CLI realtime với PCAP PMD:

```bash
sudo ./build/flowtable \
  -l 0-4 --vdev 'net_pcap0,rx_pcap=traffic.pcap,infinite_rx=1' -- \
  --mode ethdev --port 0 --workers 4 --max-workers 4 --packets 0 --cli
```

Lệnh ngắn hơn, tự kiểm tra hugepages và tự sinh PCAP nếu thiếu:

```bash
scripts/run_cli_pcap.sh
```

Ví dụ bật dashboard realtime:

```bash
sudo ./build/flowtable \
  -l 0-4 --vdev 'net_pcap0,rx_pcap=traffic.pcap,infinite_rx=1' -- \
  --mode ethdev --port 0 --workers 4 --packets 0 --dashboard
```

## Rule và xác định hướng

- [config/spi_rules.csv](config/spi_rules.csv) chứa 13 SPI filters được chuyển
  từ Excel, precedence theo filter group và một default rule.
- Workbook không cung cấp action, nên rule nhập từ Excel được gán `FORWARD`.
- [config/direction_rules.csv](config/direction_rules.csv) chứa các strategy
  xác định hướng. Cấu hình mẫu hỗ trợ Ethernet không VLAN bằng subnet
  `10.0.0.0/8`, đồng thời giữ VLAN 100/200 như một ví dụ tùy chọn.

DOCX không yêu cầu VLAN hay frame Wi-Fi 802.11. Packet Ethernet tagged và
untagged đều được hỗ trợ. Traffic từ Wi-Fi dùng được khi AP/router đã bridge
sang Ethernet; Radiotap/802.11 monitor-mode frame chưa được parse.

Thứ tự ưu tiên resolver là:

```text
explicit packet hint -> ingress port -> VLAN -> source/destination prefix
```

Định dạng [direction_rules.csv](config/direction_rules.csv):

```text
match_type,value,tenant_id,direction
SRC_PREFIX,10.0.0.0/8,1,UPLINK
DST_PREFIX,10.0.0.0/8,1,DOWNLINK
VLAN,100,1,UPLINK
INGRESS_PORT,1,1,DOWNLINK
```

Chỉ bật những strategy phản ánh đúng topology triển khai. Ví dụ, không cấu
hình `INGRESS_PORT,0,1,UPLINK` nếu cùng port 0 nhận cả hai chiều.

Nếu không strategy nào match, hệ thống vẫn tạo symmetric key bằng thứ tự
endpoint, nhưng direction là `UNKNOWN` và ngữ nghĩa client/server không được
bảo đảm.

## Cấu trúc repo

```text
include/       Public data structures và API
src/config.c   CLI/runtime config parser
src/packet.c   Ethernet/VLAN/IPv4/TCP/UDP parser và flow normalization
src/flow.c     Per-worker flow table, lifecycle counters và aging
src/rule.c     SPI rule loader, precedence, action và traffic classifier
src/port.c     Ethdev/PCAP PMD port and queue setup
src/dispatcher.c RX burst, parse, canonical hash and ring enqueue
src/worker.c   Worker loop, flow lookup, SPI cache, action counters
src/control.c  Signals, runtime CLI and rule reload control
src/pipeline.c Ethdev orchestration, lcore launch and cleanup
src/stats.c    Live CLI/dashboard/reporting counters and graphs
tests/         Unit test, test data và workbook test cases
config/        SPI rules và direction strategies
scripts/       Tạo workbook và chạy benchmark
docs/          SRS, HLD, flow diagrams, detailed design, code guide
reports/       Kết quả benchmark đo trên máy hiện tại
```

## Trạng thái xác minh

- Build DPDK 25.11.1: đạt
- Unit tests: 127/127 đạt
- Ethdev/PCAP path: smoke-test 10/10 VLAN/TCP packet, một bidirectional flow;
  NIC vật lý và line-rate vẫn cần test trên server đích
