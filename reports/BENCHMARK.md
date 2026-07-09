# Test and Benchmark Report

Ngày đo gần nhất: 2026-07-09.

## Môi trường

- CPU: Intel Core i5-8250U, 4 physical cores / 8 threads
- NUMA: 1 node
- DPDK: 25.11.1
- Compiler: GCC 16.1.1, `-O3`
- Chế độ benchmark hiện tại: hugepages 2MB, 512 pages, `--huge-dir /mnt/huge`,
  `--in-memory`, `--no-pci`
- E2E benchmark: 2 warmup runs bị bỏ qua, 5 measured runs, lấy median PPS
- Workload: PCAP PMD, `infinite_rx=1`, 100.000 flow / 200.000 packet
- Dataset rule: `config/spi_rules.csv`

Đây là môi trường laptop, không phải server data-plane chuyên dụng. Turbo,
shared LLC/memory bandwidth và governor của CPU vẫn ảnh hưởng hiệu suất scale.
`--in-memory` được giữ để tránh DPDK multiprocess socket trên laptop/sandbox;
benchmark không dùng `--no-huge`.

## Functional Test

```text
tests=127 passed=127 failed=0
```

Các invariant đã tự động kiểm:

- UL/DL cùng canonical key và hash;
- direction UL/DL đúng cho cả VLAN strategy và Ethernet untagged theo subnet;
- rule precedence/action;
- lookup không tạo flow trùng;
- timeout xóa và cập nhật counters;
- phân phối 100.000 flow lên bốn worker cân bằng;
- parser VLAN/IPv4/TCP trên DPDK mbuf;
- tenant isolation, unknown-direction symmetric fallback và table-full behavior;
- đủ sáu traffic classes.

PCAP PMD smoke test phát 10 packet VLAN/TCP gồm năm cặp UL/DL của cùng phiên:

```text
dispatched=10 processed=10 active_flows=1 dropped=0
```

## End-to-End Benchmark

Nguồn chính xác: `reports/e2e_benchmark_results.csv`. CSV lưu
`warmup_runs`, `measure_runs`, `median_run`, flow lifecycle counters,
flow-create-rate và lcore allocation. PPS trong bảng là tổng thông lượng packet
đã xử lý của toàn pipeline ở median run, không phải trung bình mỗi worker.

Workload PCAP giữ tỉ lệ benchmark phổ biến: 100.000 flow và khoảng hai packet
cho mỗi flow. Quy trình chạy warmup process trước và lấy median của 5 lần đo
để giữ đủ pipeline thật: PCAP PMD với `infinite_rx=1`, parser, direction
resolver, SPI rule/cache, dispatcher, ring và worker flow table.

| Profile | Mode | Workers | RXQ/Dispatchers | Warmup/Measured | Packets | Processed | Dropped | Active flow | PPS |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| pcap-spi-4w-huge | ethdev/PCAP PMD | 4/4 | 1/1 | 2/5 | 200,000 | 200,000 | 0 | 100,000 | 5.46 Mpps |
| pcap-spi-mq-2d4w-huge | ethdev/PCAP PMD shards | 4/4 | 2/2 | 2/5 | 200,000 | 200,000 | 0 | 100,000 | 7.24 Mpps |

Các trường bắt buộc theo mục "3. Bài test & kết quả" trong docx:

| Profile | Throughput | Max active flows | Flow create rate | Flow delete/timeout rate | CPU core usage |
|---|---:|---:|---:|---:|---|
| pcap-spi-4w-huge | 5.46 Mpps | 100,000 | 2.73 Mflows/s | 0/s | 5 lcores allocated, 62.50% of 8 logical CPUs |
| pcap-spi-mq-2d4w-huge | 7.24 Mpps | 100,000 | 3.62 Mflows/s | 0/s | 7 lcores allocated, 87.50% of 8 logical CPUs |

CPU ở bảng trên là mức core/lcore được cấp phát cho EAL command, không phải
OS-sampled utilization từ `pidstat`/`perf`. DPDK poll-mode thường giữ lcore
active trong measured window; nếu cần CPU utilization kiểu hệ điều hành, lần
benchmark trên server nên thu thêm `pidstat -t` hoặc `perf stat` song song.
Flow delete/timeout bằng 0 vì workload 200k packet kết thúc trước timeout và
không có aging pressure test trong run hiện tại.

`pcap-spi-4w-huge` dùng PCAP Ethernet được sinh từ `SPI_DPI_rule.xlsx` qua
`scripts/generate_spi_pcap.py`. Profile `pcap-spi-mq-2d4w-huge` dùng hai RX
queue, hai dispatcher và hai PCAP shard độc lập; mỗi dispatcher có SPSC ring
riêng tới từng worker, worker round-robin drain các ring đó.

## Realtime Benchmark CLI

`scripts/run_cli_pcap.sh` bật PCAP PMD với `infinite_rx=1` mặc định, nên CLI
có thể theo dõi benchmark live thay vì chỉ xem summary cuối run.

Các lệnh hữu ích:

- `show benchmark`: elapsed time, interval/average PPS, Mbps, flow-create-rate,
  interval drop và total drop.
- `show dashboard`: dashboard ANSI có bảng realtime và graph Active Flow,
  Throughput, Packet Drop.
- `show worker N`: thống kê riêng một worker core, gồm traffic classes
  `HTTP/HTTPS/DNS/TCP/UDP/OTHER`.

Chạy nhanh:

```bash
scripts/run_cli_pcap.sh
```

## Chưa đo

- NIC line rate và packet loss vật lý
- p50/p99 latency
- aging degradation sau timeout 5 giây
- dual-socket NUMA local/remote
- OS-sampled CPU utilization trong lúc benchmark
- flow delete/timeout rate dưới workload aging thật

Các trường hợp này đã có trong sheet `Performance Test` của workbook.
