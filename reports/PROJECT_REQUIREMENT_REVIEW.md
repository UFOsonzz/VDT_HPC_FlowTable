# Project Requirement Review

Đối chiếu với `MiniProject_HPC_Flowtable.docx`, trạng thái hiện tại của repo
đã khớp các nhóm yêu cầu chính. Điểm vừa bổ sung trong nhánh này là tách module
thống kê/display và thêm view chi tiết cho một worker core.

## 1. Mã nguồn và ứng dụng

| Yêu cầu docx | Trạng thái | Artifact hiện tại |
|---|---|---|
| Data plane nhận packet từ NIC hoặc PCAP | Đạt | `src/pipeline.c`, ethdev mode, PCAP PMD qua `--vdev net_pcap0,rx_pcap=...` |
| Parser Ethernet/VLAN/IPv4/TCP/UDP | Đạt | `src/packet.c`, `include/ft_packet.h` |
| Five-tuple canonical hai chiều | Đạt | `ft_packet_normalize()` tạo canonical key UL/DL |
| Flow table per worker, lock-free fast path | Đạt | `src/flow.c`; mỗi worker sở hữu shard riêng |
| Multi worker 4-8 core, flow affinity | Đạt | `src/pipeline.c`; fixed-worker hash canonical key; dynamic owner-map khi scale runtime |
| Dispatcher/RX/worker pipeline | Đạt | RX/dispatcher enqueue qua ring, worker xử lý run loop riêng |
| SPI rule engine FORWARD/DROP/LOG/COUNT | Đạt | `src/rule.c`, `config/spi_rules.csv` |
| Realtime CLI/dashboard | Đạt | `--cli`, `--dashboard`, `src/stats.c` |
| Module tách tưởng minh | Đạt sau refactor | `config.c`, `packet.c`, `flow.c`, `rule.c`, `pipeline.c`, `stats.c`, `main.c` |

Phân tách module hiện tại:

| Module | Trách nhiệm |
|---|---|
| `src/main.c` | Entry point, parse EAL/app arguments, chọn mode |
| `src/config.c` | Runtime config defaults và CLI options |
| `src/packet.c` | Parse frame, resolve direction, normalize flow key |
| `src/flow.c` | Flow table shard, create/lookup, lifecycle counters, timeout aging |
| `src/rule.c` | Load SPI CSV, match precedence, classify traffic, action name |
| `src/pipeline.c` | RX/dispatcher/worker orchestration, rings, NIC/PCAP path, control events |
| `src/stats.c` | CLI tables, dashboard, live stats, summary output |

## 2. Thống kê lưu lượng và display

Docx yêu cầu thống kê theo flow table và theo worker core. Display hiện có:

| Command | Nội dung |
|---|---|
| `show statistics` | active/launched worker, dispatched, processed, bytes, forwarded/dropped, active/created/deleted/timed-out flows, rule hits |
| `show flow` | per-worker active/capacity/created/deleted/timed-out flow |
| `show worker` | per-worker state, lcore, socket, queue, packet, byte, dropped, active flow |
| `show worker N` | chi tiết một worker core: queue/lcore/socket, packet/byte/forward/drop, flow lifecycle, `HTTP/HTTPS/DNS/TCP/UDP/OTHER`, direction counters |
| `show traffic` | aggregate direction, aggregate traffic class, rule-hit table |
| `show dashboard` / `--dashboard` | realtime dashboard ANSI, PPS/mbps interval, worker table, traffic class, top rule hits |

`show worker N` là phần còn thiếu so với ví dụ `[Worker Core 0 Statistics]`
trong docx và đã được bổ sung ở `src/stats.c`.

## 3. So sánh với flow chart

| Flow chart step | Implementation |
|---|---|
| DPDK init | `src/main.c` gọi EAL và load app config |
| Load SPI/direction rules | `rule_store_init()` và `ft_direction_config_load()` |
| Init worker cores | `create_worker()` tạo ring, mempool, flow table shard |
| RX burst | `rte_eth_rx_burst()` trong ethdev mode hoặc PCAP PMD |
| Parse packet, extract five-tuple | `ft_packet_parse_mbuf()` và `ft_packet_normalize()` |
| Determine worker | Fixed mode dùng canonical hash; dynamic mode dùng owner map |
| Enqueue to worker ring | `dispatch_enqueue_item()` theo dispatcher/worker ring |
| Worker flow lookup | `ft_flow_table_get_or_create()` |
| SPI action | Rule match/cache khi flow mới; packet sau dùng cached action |
| Drop/forward/free | Worker update action counters, optional TX, free/drop mbuf |
| Realtime stats | `src/stats.c` đọc local counters để render CLI/dashboard |

Khác biệt có chủ ý: docx minh họa worker selection bằng `SourceIP % N`, còn
repo dùng hash của canonical key. Cách này vẫn đáp ứng flow affinity, đồng thời
giữ hai chiều UL/DL cùng worker và tránh bias theo prefix IP.

## 4. Bài test và kết quả

Các trường docx yêu cầu trong mục test/result đã có artifact:

| Yêu cầu | Artifact |
|---|---|
| Functional tests và kết quả | `make test`, 127/127 pass; `tests/FlowTable_Test_Cases.xlsx` |
| Performance benchmarks và kết quả | `reports/BENCHMARK.md`, `reports/benchmark_results.csv`, `reports/e2e_benchmark_results.csv` |
| Throughput PPS | `pps` trong CSV và bảng E2E/worker-scale |
| Max active flows | `active_flows` trong E2E CSV, 100,000 flow hiện tại |
| CPU core usage % | `cpu_lcores` và `cpu_core_usage_percent` trong E2E CSV |
| Flow create/delete rate | `flow_create_rate`, `deleted_flows`, `timed_out_flows` trong E2E CSV |

Giới hạn còn lại: CPU core usage hiện là lcore allocation theo command EAL
chứ chưa phải OS-sampled utilization từ `pidstat` hoặc `perf`. Vì DPDK poll
mode giữ lcore bận trong measured window, con số này đủ để báo cáo core
allocation nhưng chưa thay thế đo CPU utilization trên server.

## 5. Advanced requirements

| Advanced item | Trạng thái | Ghi chú benchmark |
|---|---|---|
| Dynamic Worker Scaling | Có | Không dùng trong E2E fixed-worker benchmark để tránh owner-map overhead |
| Runtime rule reload | Có | `reload` CLI hoặc `SIGHUP`, áp dụng cho flow mới |
| Rule hit statistics | Có | `show traffic`, `show dashboard`, summary `rule_hit` |
| Lock-free flow table | Có ở fast path | Shard per worker, không shared flow-table lock |
| NUMA awareness | Có mức lcore/socket | Worker/ring/mempool tạo theo socket của lcore; cần dual-socket server để đo |
| CLI realtime tool | Có | `scripts/run_cli_pcap.sh` hoặc `--cli` |
| Dashboard realtime | Có | `--dashboard` hoặc `show dashboard` |

## 6. Kết luận

Project hiện đã đáp ứng yêu cầu chính và phần nâng cao trong docx ở mức code
và artifact báo cáo. Hai phần cần đo thêm nếu chuyển sang server thật là
NIC line-rate/packet loss vật lý và OS-sampled CPU utilization.
