# Test and Benchmark Report

Ngày đo: 2026-06-20.

## Môi trường

- CPU: Intel Core i5-8250U, 4 physical cores / 8 threads
- NUMA: 1 node
- DPDK: 25.11.1
- Compiler: GCC 16.1.1, `-O3`
- Chế độ: `--no-huge --in-memory --no-pci` trong sandbox
- Dataset rule: `config/spi_rules.csv`

Đây là môi trường laptop, không phải server data-plane chuyên dụng. Turbo,
shared LLC/memory bandwidth và không dùng hugepages ảnh hưởng hiệu suất scale.

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

## End-to-end synthetic pipeline

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

Nguồn chính xác: `reports/e2e_benchmark_results.csv`.

Các profile này chạy binary `build/flowtable`, vì vậy đo cả dispatcher,
normalize, owner-map, ring, worker flow table, SPI/action cache và drain worker.
`pcap-vlan` đi qua DPDK PCAP PMD, `rte_eth_rx_burst()` và parser mbuf thật.

| Profile | Mode | Workers | Packets | Processed | Dropped | Active flow | PPS |
|---|---|---:|---:|---:|---:|---:|---:|
| synthetic-fixed | synthetic | 1/1 | 50,000 | 50,000 | 0 | 5,000 | 6.43 Mpps |
| synthetic-scale | synthetic | 1 -> 2 | 50,000 | 50,000 | 0 | 20,000 | 2.98 Mpps |
| pcap-vlan | ethdev/PCAP PMD | 1/1 | 10 | 10 | 0 | 1 | 0.16 Mpps |

`synthetic-scale` bật logical dynamic scaling. Dispatcher dùng owner-map nên flow
đã có owner vẫn đi về worker cũ; worker mới chỉ nhận flow mới sau thời điểm
scale. PPS profile này không so trực tiếp với fixed profile vì số flow lớn hơn
và có thêm owner-map/control overhead.

## Worker scale benchmark

Nguồn chính xác: `reports/benchmark_results.csv`.

### High-cardinality: 32.768 flow mỗi worker

| Worker | PPS | Speedup | Parallel efficiency |
|---:|---:|---:|---:|
| 1 | 8.08 Mpps | 1.00x | 100% |
| 2 | 14.34 Mpps | 1.78x | 88.8% |
| 4 | 20.08 Mpps | 2.49x | 62.1% |

### Hot-cache: 4.096 flow mỗi worker

| Worker | PPS | Speedup | Parallel efficiency |
|---:|---:|---:|---:|
| 1 | 9.77 Mpps | 1.00x | 100% |
| 2 | 17.32 Mpps | 1.77x | 88.6% |
| 4 | 22.88 Mpps | 2.34x | 58.5% |

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
