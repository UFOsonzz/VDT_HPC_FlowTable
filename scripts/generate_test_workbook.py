#!/usr/bin/env python3
"""Generate the two-sheet Excel test catalogue without third-party packages."""

from pathlib import Path
from xml.sax.saxutils import escape
from zipfile import ZIP_DEFLATED, ZipFile


FUNCTIONAL = [
    ["ID", "Module", "Test objective", "Precondition", "Input / Steps",
     "Expected result", "Automation", "Status", "Evidence"],
    ["FT-001", "Configuration", "Load direction strategies", "Valid CSV",
     "Load config/direction_rules.csv", "VLAN and subnet rules are parsed", "Unit", "PASS",
     "make test"],
    ["FT-002", "Flow key", "Untagged UL and DL produce one key",
     "Subscriber subnet direction rules",
     "Normalize an untagged packet and its reverse", "Keys are byte-identical",
     "Unit", "PASS", "test_untagged_bidirectional_key"],
    ["FT-003", "Dispatcher", "Untagged UL and DL select one worker", "FT-002",
     "Hash both normalized keys", "Hash and worker_id are identical", "Unit",
     "PASS", "test_untagged_bidirectional_key"],
    ["FT-004", "SPI", "Both directions reuse one rule", "Existing flow",
     "Process UL then reversed DL", "Rule is matched once and cached in entry",
     "Unit/Integration", "PASS", "test_rule_reuse_for_both_directions"],
    ["FT-005", "Tenant", "Same tuple in different tenants is isolated",
     "Two direction rules", "Normalize tenant 1 and tenant 2 packets",
     "Flow keys differ by tenant_id", "Unit", "PASS",
     "test_unknown_direction_and_tenant_isolation"],
    ["FT-006", "SPI", "Rule precedence is respected", "Test rule set",
     "Match YouTube TCP/443", "YOUTUBE_ALLOW wins before generic HTTPS",
     "Unit", "PASS", "test_rule_reuse_for_both_directions"],
    ["FT-007", "SPI", "Wildcard fields match any endpoint", "DEFAULT rule",
     "Match an unmatched UDP flow", "DEFAULT/FORWARD is returned", "Unit",
     "PASS", "Rule loader + matcher"],
    ["FT-008", "SPI", "DROP action is enforced", "SSH_BLOCK rule",
     "Process TCP destination port 22", "Packet is dropped and counter rises",
     "Unit", "PASS", "test_rule_reuse_for_both_directions"],
    ["FT-009", "Flow table", "Create a new flow", "Empty table",
     "get_or_create(key)", "active=1, created=1", "Unit", "PASS",
     "test_flow_lifecycle"],
    ["FT-010", "Flow table", "Lookup does not duplicate flow", "FT-009",
     "get_or_create(same key)", "Same entry; created=false", "Unit", "PASS",
     "test_flow_lifecycle"],
    ["FT-011", "Aging", "Idle flow expires", "Timeout threshold supplied",
     "Age beyond timeout", "Flow deleted; timeout counter rises", "Unit",
     "PASS", "test_flow_lifecycle"],
    ["FT-012", "Load balance", "Hash distribution is balanced", "100k keys",
     "Distribute across four workers", "Max-min bucket delta < 1.5%",
     "Unit", "PASS", "test_worker_distribution"],
    ["FT-013", "Classifier", "HTTP/HTTPS/DNS/TCP/UDP/OTHER counters",
     "Representative packets", "Run each protocol/port combination",
     "Each packet reaches the correct class", "Unit", "PASS",
     "test_traffic_classification"],
    ["FT-014", "Parser", "Parse tagged/untagged Ethernet and IPv4 L4", "DPDK mbufs",
     "Feed tagged TCP and untagged UDP", "Five-tuple and optional VLAN are correct",
     "Unit/Integration", "PASS", "test_vlan_tcp_parser + PCAP PMD smoke"],
    ["FT-015", "Pipeline", "No packet loss in PCAP PMD pipeline",
     "4 workers, adequate capacity", "Replay generated SPI PCAP",
     "dispatched=processed and dropped=0", "Integration", "PASS",
     "reports/e2e_benchmark_results.csv"],
    ["FT-016", "Fallback", "Unknown direction still has symmetric key",
     "No direction strategy matches", "Normalize packet and reverse packet",
     "Lexical endpoint canonicalization yields one key", "Unit", "PASS",
     "test_unknown_direction_and_tenant_isolation"],
    ["FT-018", "Direction", "Optional VLAN strategy remains supported",
     "VLAN 100=UL and VLAN 200=DL", "Normalize tagged packet and reverse",
     "Directions and canonical keys are correct", "Unit", "PASS",
     "test_bidirectional_key"],
    ["FT-017", "Flow capacity", "Full shard fails safely", "Small table",
     "Insert capacity+1 unique flows", "No overwrite; extra packet is dropped",
     "Unit", "PASS", "test_flow_capacity"],
]


PERFORMANCE = [
    ["ID", "Scenario", "Workload", "Metric", "Acceptance criterion",
     "Command / Method", "Measured result", "Status", "Notes"],
    ["PT-001", "Single-dispatcher E2E", "2M processed packets, 100k generated SPI flows",
     "PPS and loss", "processed=dispatched; no unexpected drop",
     "make benchmark-e2e",
     "6.75 Mpps; zero loss; 100k active flows",
     "PASS", "200k-packet base PCAP replayed by infinite_rx=1; 2 warmup + 5 measured runs; median PPS"],
    ["PT-002", "Multi-RX E2E", "2 RX queues, 2 dispatchers, 4 workers",
     "PPS and scale", "Outperform single-dispatcher profile",
     "make benchmark-e2e",
     "10.26 Mpps; 1.52x over single-dispatcher PCAP", "PASS",
     "PCAP PMD shards; --per-dispatcher-limit"],
    ["PT-003", "Flow creation rate", "100k unique flows in E2E PCAP",
     "Flows/s", "Report create/delete rates",
     "make benchmark-e2e",
     "0.34 Mflows/s single PCAP; 0.51 Mflows/s multi-RX; delete/timeout=0/s",
     "PASS", "Current CPU field is lcore allocation: 5/8 and 7/8 logical CPUs"],
    ["PT-004", "Realtime CLI dynamic scaling", "PCAP PMD with infinite_rx=1",
     "Active worker rebalance", "scale up/down changes active workers and flow placement",
     "scripts/run_cli_pcap.sh then scale up/down",
     "Command implemented; manual visual check required", "READY",
     "Default CLI starts 2/4 workers; rebalance runs outside fixed-worker benchmark"],
    ["PT-005", "Dashboard graphs", "Running CLI or --dashboard mode",
     "Graph visibility", "Active Flow, Throughput and Packet Drop are rendered",
     "show dashboard", "Implemented", "READY", "ASCII history, 32 samples"],
    ["PT-006", "Aging pressure", "90% idle flows; timeout scan active",
     "PPS degradation", "<10% throughput loss", "Run for >5 seconds",
     "Not measured", "NOT RUN", ""],
    ["PT-007", "Ring backpressure", "Bursty RX beyond worker service rate",
     "Drop rate", "No silent loss; counters reconcile", "Traffic generator",
     "Not measured", "NOT RUN", ""],
    ["PT-008", "NUMA locality", "Local vs remote allocation on 2-socket host",
     "PPS/latency delta", "Local allocation outperforms remote",
     "Run with explicit lcore/socket maps", "Host has one NUMA node",
     "N/A", "Requires dual-socket server"],
    ["PT-009", "PCAP PMD replay", "Representative traffic.pcap",
     "PPS/rule accuracy", "No parse errors; expected rule hits",
     "--vdev net_pcap0 + --mode ethdev", "Not measured", "NOT RUN",
     "Requires a supplied PCAP"],
]


def cell_xml(column, row, value, style=0):
    ref = f"{column}{row}"
    if isinstance(value, (int, float)):
        return f'<c r="{ref}" s="{style}"><v>{value}</v></c>'
    text = escape(str(value))
    return (
        f'<c r="{ref}" s="{style}" t="inlineStr">'
        f'<is><t xml:space="preserve">{text}</t></is></c>'
    )


def column_name(index):
    result = ""
    while index:
        index, remainder = divmod(index - 1, 26)
        result = chr(65 + remainder) + result
    return result


def sheet_xml(rows):
    rendered_rows = []
    for row_index, row in enumerate(rows, 1):
        cells = []
        for column_index, value in enumerate(row, 1):
            style = 1 if row_index == 1 else 2
            cells.append(cell_xml(column_name(column_index), row_index,
                                  value, style))
        rendered_rows.append(
            f'<row r="{row_index}" ht="30" customHeight="1">'
            + "".join(cells) + "</row>"
        )
    last_column = column_name(max(len(row) for row in rows))
    widths = "".join(
        f'<col min="{i}" max="{i}" width="{18 if i != 5 else 34}" customWidth="1"/>'
        for i in range(1, len(rows[0]) + 1)
    )
    return f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <sheetViews><sheetView workbookViewId="0"><pane ySplit="1" topLeftCell="A2"
  activePane="bottomLeft" state="frozen"/></sheetView></sheetViews>
  <cols>{widths}</cols>
  <sheetData>{''.join(rendered_rows)}</sheetData>
  <autoFilter ref="A1:{last_column}{len(rows)}"/>
</worksheet>"""


def generate(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(path, "w", ZIP_DEFLATED) as archive:
        archive.writestr("[Content_Types].xml", """<?xml version="1.0"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
 <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
 <Default Extension="xml" ContentType="application/xml"/>
 <Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>
 <Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
 <Override PartName="/xl/worksheets/sheet2.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
 <Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>
</Types>""")
        archive.writestr("_rels/.rels", """<?xml version="1.0"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
 <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>
</Relationships>""")
        archive.writestr("xl/workbook.xml", """<?xml version="1.0"?>
<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"
 xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
 <sheets>
  <sheet name="Functional Test" sheetId="1" r:id="rId1"/>
  <sheet name="Performance Test" sheetId="2" r:id="rId2"/>
 </sheets>
</workbook>""")
        archive.writestr("xl/_rels/workbook.xml.rels", """<?xml version="1.0"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
 <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>
 <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet2.xml"/>
 <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
</Relationships>""")
        archive.writestr("xl/styles.xml", """<?xml version="1.0"?>
<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
 <fonts count="2"><font><sz val="11"/><name val="Calibri"/></font>
 <font><b/><color rgb="FFFFFFFF"/><sz val="11"/><name val="Calibri"/></font></fonts>
 <fills count="3"><fill><patternFill patternType="none"/></fill>
 <fill><patternFill patternType="gray125"/></fill>
 <fill><patternFill patternType="solid"><fgColor rgb="FF1F4E78"/><bgColor indexed="64"/></patternFill></fill></fills>
 <borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>
 <cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>
 <cellXfs count="3">
  <xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>
  <xf numFmtId="0" fontId="1" fillId="2" borderId="0" xfId="0" applyFill="1" applyFont="1"><alignment wrapText="1" vertical="center"/></xf>
  <xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"><alignment wrapText="1" vertical="top"/></xf>
 </cellXfs>
 <cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles>
</styleSheet>""")
        archive.writestr("xl/worksheets/sheet1.xml", sheet_xml(FUNCTIONAL))
        archive.writestr("xl/worksheets/sheet2.xml", sheet_xml(PERFORMANCE))


if __name__ == "__main__":
    output = Path("tests/FlowTable_Test_Cases.xlsx")
    generate(output)
    print(f"Wrote {output}")
