# FlowTable DPDK

FlowTable là data plane IPv4 hiệu năng cao dùng DPDK, được xây dựng từ
`MiniProject_HPC_Flowtable.docx` và sheet `SPI_rule` trong
`SPI_DPI_rule.xlsx`.

Thiết kế dùng pipeline và ownership theo core:

1. RX/PCAP/synthetic ingress nhận packet theo burst.
2. Parser lấy Ethernet, VLAN tùy chọn, IPv4, TCP/UDP.
3. Direction Resolver xác định tenant và uplink/downlink từ metadata, ingress
   port, VLAN hoặc IP subnet.
4. Five-tuple được chuẩn hóa thành khóa client/server hai chiều.
5. Symmetric hash đưa cả hai chiều vào cùng SPSC ring và cùng worker.
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
make benchmark
```

## Chạy synthetic pipeline

Ví dụ bốn worker, một triệu packet, 100.000 flow hai chiều:

```bash
XDG_RUNTIME_DIR=/tmp ./build/flowtable \
  -l 0-4 --no-huge --in-memory --no-pci --no-telemetry -- \
  --mode synthetic --workers 4 --packets 1000000 --flows 100000 \
  --flow-capacity 32768
```

`--no-huge` chỉ phù hợp để phát triển. Khi chạy production, bỏ cờ này, cấu hình
hugepages và dùng EAL arguments tương ứng máy chủ.

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
src/           Parser, canonical key, rule engine, flow table, pipeline
bench/         Benchmark scale theo core
tests/         Unit test, test data và workbook test cases
config/        SPI rules và direction strategies
scripts/       Tạo workbook và chạy benchmark
docs/          SRS, HLD, flow diagrams, detailed design, code guide
reports/       Kết quả benchmark đo trên máy hiện tại
```

## Trạng thái xác minh

- Build DPDK 25.11.1: đạt
- Unit tests: 127/127 đạt
- Synthetic pipeline: 1.000.000/1.000.000 packet xử lý, 100.000 active flow
- Ethdev/PCAP path: smoke-test 10/10 VLAN/TCP packet, một bidirectional flow;
  NIC vật lý và line-rate vẫn cần test trên server đích
