# Test and Benchmark Report

Ngày đo gần nhất: 2026-07-09.

## Môi trường

- CPU: Intel Core i5-8250U, 4 physical cores / 8 threads
- NUMA: 1 node
- DPDK: 25.11.1
- Compiler: GCC 16.1.1, `-O3`
- Chế độ benchmark hiện tại: hugepages 2MB, 512 pages, `--huge-dir /mnt/huge`,
  `--in-memory`, `--no-pci`
- E2E benchmark: 2 warmup runs bị bỏ qua, 5 measured runs, lấy median PPS;
  workload PCAP mặc định 1.000.000 packet
- Dataset rule: `config/spi_rules.csv`

Đây là môi trường laptop, không phải server data-plane chuyên dụng. Turbo,
shared LLC/memory bandwidth và governor của CPU vẫn ảnh hưởng hiệu suất scale.
`--in-memory` được giữ để tránh DPDK multiprocess socket trên laptop/sandbox;
benchmark không dùng `--no-huge`.

## Functional test

```text
tests=127 passed=127 failed=0
```

Các invariant đã tự động kiểm:

- UL/DL cùng canonical key và hash;
- direction UL/DL đúng cho cả VLAN strategy và Ethernet untagged theo subnet;
- rule precedence/action;
- lookup không tạo flow trùng;
- timeout xóa và cập nhật counters;
- phân phối 100.000 flow lên bốn worker cân bằng.
- parser VLAN/IPv4/TCP trên DPDK mbuf;
- tenant isolation, unknown-direction symmetric fallback và table-full behavior;
- đủ sáu traffic classes.

PCAP PMD smoke test phát 10 packet VLAN/TCP gồm năm cặp UL/DL của cùng phiên:

```text
dispatched=10 processed=10 active_flows=1 dropped=0
```

## Development smoke synthetic pipeline

Đây là smoke benchmark lịch sử cho chế độ phát triển `--no-huge`. Benchmark
hiện tại ở các mục bên dưới đã chuyển sang hugepages.

Command:

```bash
XDG_RUNTIME_DIR=/tmp ./build/flowtable \
  -l 0-4 --no-huge --in-memory --no-pci --no-telemetry -- \
  --workers 4 --packets 1000000 --flows 100000 \
  --flow-capacity 32768
```

Kết quả:

```text
dispatched=1,000,000
processed=1,000,000
forwarded=1,000,000
dropped=0
active_flows=100,000
median seconds=0.352795
median throughput=2.84 Mpps
```

Phân phối flow: 25.071 / 24.910 / 25.022 / 24.997; sai lệch rất nhỏ.

Số end-to-end trên được đo lại sau khi Direction Resolver hỗ trợ Ethernet
untagged bằng subnet. Ba lần chạy gần nhất đạt 2.38 / 2.83 / 2.85 Mpps; máy
laptop đang chạy môi trường IDE nên con số này chỉ dùng làm smoke benchmark.
Invariant quan trọng là đủ 1.000.000 packet, đúng 100.000 bidirectional flow và
không mất gói. Worker-engine benchmark bên dưới tách khỏi dispatcher/resolver.

## End-to-end benchmark hiện tại

Nguồn chính xác: `reports/e2e_benchmark_results.csv`. CSV lưu cả
`warmup_runs`, `measure_runs` và `median_run`. PPS trong bảng là tổng thông
lượng packet đã xử lý của toàn pipeline ở median run, không phải trung bình
mỗi worker.

Benchmark cũ chạy một lần với 200.000 packet nên nhiều profile kết thúc trong
vài chục ms; số đo dễ bị ảnh hưởng bởi CPU frequency, page cache, branch/cache
cold start và nhiễu nền của laptop. Quy trình hiện tại chạy warmup process
trước, tăng workload đo lên 1.000.000 packet và lấy median của 5 lần đo để
giữ đủ pipeline thật: packet generation/PCAP PMD, parser, direction resolver,
SPI rule/cache, dispatcher, ring và worker flow table. Không dùng app-level
"warm flow" reset vì cách đó sẽ bỏ bớt chi phí tạo flow/rule match ban đầu và
làm benchmark lạc quan hơn workload end-to-end thật.

Các profile này chạy binary `build/flowtable`, vì vậy đo cả dispatcher,
normalize, flow-affinity dispatch, ring, worker flow table, SPI/action cache và
drain worker. PCAP profile đi qua DPDK PCAP PMD, `rte_eth_rx_burst()` và parser
mbuf thật.

| Profile | Mode | Workers | RXQ/Dispatchers | Warmup/Measured | Packets | Processed | Dropped | Active flow | PPS |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| synthetic-fixed-huge | synthetic | 1/1 | 0/0 | 2/5 | 1,000,000 | 1,000,000 | 0 | 20,000 | 6.16 Mpps |
| synthetic-scale-huge | synthetic | 1 -> 4 | 0/0 | 2/5 | 1,000,000 | 1,000,000 | 0 | 100,000 | 2.36 Mpps |
| pcap-spi-4w-huge | ethdev/PCAP PMD | 4/4 | 1/1 | 2/5 | 1,000,000 | 1,000,000 | 0 | 100,000 | 1.95 Mpps |
| pcap-spi-mq-2d4w-huge | ethdev/PCAP PMD shards | 4/4 | 2/2 | 2/5 | 1,000,000 | 1,000,000 | 0 | 100,000 | 2.63 Mpps |

`synthetic-scale-huge` bật logical dynamic scaling. Dispatcher dùng owner-map
nên flow đã có owner vẫn đi về worker cũ; worker mới chỉ nhận flow mới sau thời
điểm scale. PPS profile này không so trực tiếp với fixed profile vì số flow lớn
hơn và có thêm owner-map/control overhead. `pcap-spi-4w-huge` dùng PCAP Ethernet
được sinh từ `SPI_DPI_rule.xlsx` qua `scripts/generate_spi_pcap.py`; profile
`pcap-spi-4w-huge` chạy bốn active worker đúng yêu cầu multi-worker. Các
profile fixed-worker dùng `--fixed-workers`, nên dispatcher chọn worker bằng
hash canonical key trực tiếp; profile scale động vẫn dùng owner-map để flow cũ
không bị đổi worker khi active worker count thay đổi. Profile
`pcap-spi-mq-2d4w-huge` dùng hai RX queue, hai dispatcher và hai PCAP shard
độc lập; mỗi dispatcher có SPSC ring riêng tới từng worker, worker round-robin
drain các ring đó. Trên laptop này profile multi-RX đạt 1.35x so với single
dispatcher PCAP PMD, khá hơn single-dispatcher trong môi trường laptop/PCAP
nhưng vẫn chưa thay thế
benchmark NIC RSS vật lý vì PCAP PMD không advertise RSS offloads.
Fast path hiện gom work-item allocation/enqueue theo burst và worker trả
work-item về mempool theo bulk, đồng thời timestamp RX được lấy theo burst thay
vì từng packet.

## Worker scale benchmark

Nguồn chính xác: `reports/benchmark_results.csv`.

### High-cardinality: 32.768 flow mỗi worker

| Worker | PPS | Speedup | Parallel efficiency |
|---:|---:|---:|---:|
| 1 | 21.28 Mpps | 1.00x | 100% |
| 2 | 41.44 Mpps | 1.95x | 97.4% |
| 4 | 57.23 Mpps | 2.69x | 67.2% |

### Hot-cache: 4.096 flow mỗi worker

| Worker | PPS | Speedup | Parallel efficiency |
|---:|---:|---:|---:|
| 1 | 22.51 Mpps | 1.00x | 100% |
| 2 | 45.62 Mpps | 2.03x | 101.3% |
| 4 | 66.66 Mpps | 2.96x | 74.0% |

Throughput tăng theo core nhưng chưa tuyến tính ở bốn core trên laptop này.
Thiết kế shard không có shared flow lock; giới hạn còn lại chủ yếu là all-core
frequency, cache/memory và platform. Production acceptance target là >=3.2x
cho bốn physical cores trên server đã pin core, fixed governor và hugepages.

## Lỗi hiệu năng đã phát hiện và sửa

Benchmark ban đầu cập nhật checksum trong các worker context nằm gần nhau,
gây false sharing. Context sau đó được cache-line aligned và checksum chỉ ghi
một lần khi kết thúc. Đây cũng là lý do mọi per-core hot counter trong data
plane phải local/cache aligned.

## Chưa đo

- NIC line rate và packet loss vật lý
- p50/p99 latency
- aging degradation sau timeout 5 giây
- dual-socket NUMA local/remote
- flow create/delete rate với 100% new-flow traffic

Các trường hợp này đã có trong sheet `Performance Test` của workbook.
