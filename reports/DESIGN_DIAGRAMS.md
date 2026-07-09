# Design Diagrams

Các block dưới đây là Mermaid code, có thể render trực tiếp trên GitHub,
Mermaid Live Editor hoặc các công cụ hỗ trợ Mermaid. Nội dung bám theo
implementation hiện tại trong repo.

## Flow Table Design

```mermaid
flowchart LR
    W[Worker Core N] --> FT[Per-worker Flow Table Shard]
    FT --> H[rte_hash canonical flow key to index]
    FT --> E[Flow Entry Array]
    FT --> F[Free Stack]
    FT --> A[Aging Cursor]
    E --> S[State: last_seen action rule_id counters]
    A --> T{Idle longer than timeout?}
    T -- yes --> D[Delete entry, decrement active, increment timed out]
    T -- no --> K[Keep flow active]
    F --> C[Allocate entry for new flow]
    C --> E
```

## Dispatcher Algorithm

```mermaid
flowchart TD
    RX[RX Burst from NIC or PCAP PMD] --> P[Parse Ethernet VLAN IPv4 TCP UDP]
    P --> V{Valid IPv4 L4 packet?}
    V -- no --> FREE[Free or drop mbuf]
    V -- yes --> N[Normalize direction and canonical key]
    N --> M{Fixed workers?}
    M -- yes --> H[worker_id = hash canonical_key mod active_workers]
    M -- no --> O[Lookup or create owner in dispatcher owner map]
    H --> Q[Select dispatcher to worker SPSC ring]
    O --> Q
    Q --> B{Ring burst has room?}
    B -- yes --> ENQ[Enqueue work item]
    B -- no --> DROP[Drop and increment ring drop counter]
    ENQ --> STAT[Increment dispatched counter]
```

## SPI Rule Engine

```mermaid
flowchart TD
    PKT[Worker receives work item] --> LOOKUP[Flow lookup or create]
    LOOKUP --> NEW{New flow?}
    NEW -- no --> CACHE[Use cached action and traffic class]
    NEW -- yes --> RULES[Scan SPI rules by precedence]
    RULES --> MATCH{Rule match?}
    MATCH -- yes --> ACT[Store rule_id action traffic_class in flow]
    MATCH -- no --> DEF[Use DEFAULT action]
    ACT --> APPLY[Apply FORWARD DROP LOG COUNT]
    DEF --> APPLY
    CACHE --> APPLY
    APPLY --> CNT[Update packet byte rule traffic direction counters]
```

## Data Flow Diagram Level 0

```mermaid
flowchart LR
    SRC[Traffic Source: NIC or PCAP] --> APP[FlowTable DPDK Application]
    RULES[(SPI and Direction Rules)] --> APP
    APP --> OUT[Forwarded packets or freed mbufs]
    APP --> MON[CLI Dashboard Benchmark CSV]
```

## Data Flow Diagram Level 1

```mermaid
flowchart LR
    SRC[Ethernet Frames] --> RX[RX Module]
    RX --> PARSER[Packet Parser]
    PARSER --> DIR[Direction Resolver]
    DIR --> KEY[Canonical Five Tuple]
    KEY --> DISP[Dispatcher]
    DISP --> RINGS[Worker Rings]
    RINGS --> WORKER[Worker Core]
    WORKER --> FLOW[(Flow Table Shard)]
    WORKER --> SPI[SPI Rule Engine]
    SPI --> FLOW
    WORKER --> ACTION[Forward Drop Log Count]
    ACTION --> SINK[TX or Free]
    WORKER --> COUNTERS[(Local Counters)]
    COUNTERS --> STATS[Stats Collector]
    STATS --> CLI[CLI Dashboard Reports]
    RULES[(Rule CSV)] --> SPI
    DCFG[(Direction CSV)] --> DIR
```

## Packet Processing Sequence

```mermaid
sequenceDiagram
    participant RX as RX or Dispatcher Core
    participant Parser as Parser and Direction Resolver
    participant Ring as Worker Ring
    participant Worker as Worker Core
    participant Flow as Flow Table
    participant SPI as SPI Engine
    participant Stats as Stats Module
    participant CLI as CLI or Dashboard

    RX->>Parser: parse mbuf and normalize key
    Parser-->>RX: canonical key, direction, traffic metadata
    RX->>RX: select worker by canonical hash or owner map
    RX->>Ring: enqueue work item
    Ring-->>Worker: dequeue burst
    Worker->>Flow: lookup or create flow
    alt new flow
        Worker->>SPI: match SPI rules
        SPI-->>Worker: action, rule_id, traffic_class
        Worker->>Flow: cache action in flow entry
    else existing flow
        Flow-->>Worker: cached action
    end
    Worker->>Worker: apply action and update local counters
    CLI->>Stats: show statistics or show worker N
    Stats-->>CLI: tables and realtime dashboard values
```

## Application Activity Diagram

```mermaid
flowchart TD
    START([Start]) --> EAL[Initialize DPDK EAL]
    EAL --> CFG[Parse app config]
    CFG --> RULE[Load direction and SPI rules]
    RULE --> INIT[Create workers, rings, mempools, flow tables]
    INIT --> MODE{Mode}
    MODE -- ethdev --> PORT[Configure NIC or PCAP PMD]
    MODE -- synthetic --> SYN[Generate synthetic packet metadata]
    PORT --> RX[RX burst loop]
    SYN --> RX
    RX --> PROC[Dispatch packets to workers]
    PROC --> CTRL{Control event?}
    CTRL -- reload --> RELOAD[Reload rules for new flows]
    CTRL -- stats --> SHOW[Render live stats/dashboard]
    CTRL -- none --> RUN{Stop condition reached?}
    RELOAD --> RUN
    SHOW --> RUN
    RUN -- no --> RX
    RUN -- yes --> DRAIN[Drain rings and stop workers]
    DRAIN --> SUM[Print summary and benchmark metrics]
    SUM --> END([Exit])
```

## Realtime Statistics Data Path

```mermaid
flowchart LR
    W0[Worker 0 local counters] --> COL[Stats collector]
    W1[Worker 1 local counters] --> COL
    WN[Worker N local counters] --> COL
    FT[Flow table lifecycle counters] --> COL
    RH[Rule hit counters] --> COL
    COL --> LIVE[live stats line]
    COL --> DASH[ANSI dashboard]
    COL --> CLI1[show statistics]
    COL --> CLI2[show worker]
    COL --> CLI3[show worker N]
    COL --> CLI4[show traffic]
    COL --> SUM[final summary for benchmark]
```
