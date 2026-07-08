#!/usr/bin/env python3
import argparse
import ipaddress
import random
import struct
import time
import zipfile
from pathlib import Path
from xml.etree import ElementTree as ET


NS_MAIN = "{http://schemas.openxmlformats.org/spreadsheetml/2006/main}"
NS_REL = "{http://schemas.openxmlformats.org/officeDocument/2006/relationships}"
LINKTYPE_ETHERNET = 1


def is_any(value):
    if value is None:
        return True
    text = str(value).strip()
    return text == "" or text.lower() in {"na", "n/a", "any", "*"}


def column_name(cell_ref):
    name = []
    for char in cell_ref:
        if char.isalpha():
            name.append(char)
        else:
            break
    return "".join(name)


def load_shared_strings(workbook):
    if "xl/sharedStrings.xml" not in workbook.namelist():
        return []
    root = ET.fromstring(workbook.read("xl/sharedStrings.xml"))
    strings = []
    for item in root.findall(NS_MAIN + "si"):
        parts = []
        for text in item.iter(NS_MAIN + "t"):
            if text.text:
                parts.append(text.text)
        strings.append("".join(parts))
    return strings


def sheet_paths(workbook):
    root = ET.fromstring(workbook.read("xl/workbook.xml"))
    rels = ET.fromstring(workbook.read("xl/_rels/workbook.xml.rels"))
    rel_map = {rel.attrib["Id"]: rel.attrib["Target"] for rel in rels}
    result = {}
    for sheet in root.find(NS_MAIN + "sheets"):
        name = sheet.attrib["name"]
        rel_id = sheet.attrib[NS_REL + "id"]
        result[name] = "xl/" + rel_map[rel_id].lstrip("/")
    return result


def cell_value(cell, shared_strings):
    value = cell.find(NS_MAIN + "v")
    if value is None:
        return ""
    text = value.text or ""
    if cell.attrib.get("t") == "s":
        return shared_strings[int(text)]
    return text


def load_spi_rules_from_xlsx(path):
    rules = []
    with zipfile.ZipFile(path) as workbook:
        shared_strings = load_shared_strings(workbook)
        paths = sheet_paths(workbook)
        if "SPI_rule" not in paths:
            raise RuntimeError("SPI_rule sheet not found")
        root = ET.fromstring(workbook.read(paths["SPI_rule"]))
        for row in root.iter(NS_MAIN + "row"):
            row_id = int(row.attrib.get("r", "0"))
            if row_id <= 1:
                continue
            cells = {}
            for cell in row.findall(NS_MAIN + "c"):
                cells[column_name(cell.attrib["r"])] = cell_value(cell, shared_strings)
            name = cells.get("B", "").strip()
            if not name:
                continue
            rules.append({
                "name": name,
                "group": cells.get("C", "").strip(),
                "dst_prefix": cells.get("D", "").strip(),
                "dst_ip": cells.get("E", "").strip(),
                "dst_port": cells.get("F", "").strip(),
                "protocol": cells.get("G", "").strip(),
                "src_ip": cells.get("H", "").strip(),
                "src_prefix": cells.get("I", "").strip(),
                "src_port": cells.get("J", "").strip(),
            })
    return rules


def choose_ip(prefix, exact, fallback, offset):
    if not is_any(exact):
        return int(ipaddress.IPv4Address(exact))
    if not is_any(prefix):
        network = ipaddress.IPv4Network(prefix, strict=False)
        usable = max(int(network.num_addresses) - 2, 1)
        return int(network.network_address) + 1 + (offset % usable)
    return int(ipaddress.IPv4Address(fallback)) + offset


def choose_protocol(rule):
    protocol = str(rule["protocol"]).strip().lower()
    group = str(rule["group"]).lower()
    port = str(rule["dst_port"]).strip()
    if protocol == "tcp":
        return 6
    if protocol == "udp":
        return 17
    if port == "53" or "dns" in group:
        return 17
    return 6


def choose_dst_port(rule, protocol):
    port = str(rule["dst_port"]).strip().lower()
    group = str(rule["group"]).lower()
    if not is_any(port):
        return int(port)
    if "dns" in group:
        return 53
    if "http" in group and "https" not in group:
        return 80
    if protocol == 6:
        return 443
    return 123


def ip_checksum(data):
    if len(data) % 2:
        data += b"\x00"
    total = 0
    for index in range(0, len(data), 2):
        total += (data[index] << 8) + data[index + 1]
        total = (total & 0xffff) + (total >> 16)
    return (~total) & 0xffff


def build_ipv4_header(src_ip, dst_ip, protocol, payload_len, ident):
    version_ihl = 0x45
    tos = 0
    total_length = 20 + payload_len
    flags_fragment = 0x4000
    ttl = 64
    checksum = 0
    header = struct.pack(
        "!BBHHHBBHII",
        version_ihl,
        tos,
        total_length,
        ident & 0xffff,
        flags_fragment,
        ttl,
        protocol,
        checksum,
        src_ip,
        dst_ip,
    )
    checksum = ip_checksum(header)
    return struct.pack(
        "!BBHHHBBHII",
        version_ihl,
        tos,
        total_length,
        ident & 0xffff,
        flags_fragment,
        ttl,
        protocol,
        checksum,
        src_ip,
        dst_ip,
    )


def build_l4(protocol, src_port, dst_port):
    if protocol == 17:
        return struct.pack("!HHHH", src_port, dst_port, 8, 0)
    data_offset_flags = (5 << 12) | 0x018
    return struct.pack("!HHIIHHHH", src_port, dst_port, 0, 0, data_offset_flags, 65535, 0, 0)


def build_ethernet(vlan_id, src_ip, dst_ip, protocol, src_port, dst_port, ident):
    dst_mac = b"\x02\x00\x00\x00\x00\x02"
    src_mac = b"\x02\x00\x00\x00\x00\x01"
    l4 = build_l4(protocol, src_port, dst_port)
    ipv4 = build_ipv4_header(src_ip, dst_ip, protocol, len(l4), ident)
    if vlan_id is None:
        ether = dst_mac + src_mac + struct.pack("!H", 0x0800)
    else:
        ether = dst_mac + src_mac + struct.pack("!HHH", 0x8100, vlan_id & 0x0fff, 0x0800)
    return ether + ipv4 + l4


def flow_tuple(rule, flow_id, rng):
    protocol = choose_protocol(rule)
    client_ip = choose_ip(rule["src_prefix"], rule["src_ip"], "10.0.0.1", flow_id)
    if is_any(rule["src_prefix"]) and is_any(rule["src_ip"]):
        client_ip = int(ipaddress.IPv4Address("10.0.0.1")) + (flow_id % 0x00fffffe)
    server_ip = choose_ip(rule["dst_prefix"], rule["dst_ip"], "198.51.100.1", flow_id)
    client_port = 1024 + (flow_id % 50000)
    if not is_any(rule["src_port"]):
        client_port = int(rule["src_port"])
    server_port = choose_dst_port(rule, protocol)
    return client_ip, server_ip, client_port, server_port, protocol


def write_pcap(path, packets):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as output:
        output.write(struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, LINKTYPE_ETHERNET))
        base_ts = int(time.time())
        for index, packet in enumerate(packets):
            ts_sec = base_ts + index // 1000000
            ts_usec = index % 1000000
            output.write(struct.pack("<IIII", ts_sec, ts_usec, len(packet), len(packet)))
            output.write(packet)


def generate_packets(rules, flow_count, packet_count, use_vlan, seed):
    rng = random.Random(seed)
    active_rules = [rule for rule in rules if rule["name"].upper() != "DEFAULT"]
    if not active_rules:
        raise RuntimeError("no SPI rules found")
    for packet_id in range(packet_count):
        flow_id = (packet_id // 2) % flow_count
        rule = active_rules[flow_id % len(active_rules)]
        client_ip, server_ip, client_port, server_port, protocol = flow_tuple(rule, flow_id, rng)
        downlink = packet_id & 1
        if downlink:
            src_ip, dst_ip = server_ip, client_ip
            src_port, dst_port = server_port, client_port
            vlan_id = 200 if use_vlan else None
        else:
            src_ip, dst_ip = client_ip, server_ip
            src_port, dst_port = client_port, server_port
            vlan_id = 100 if use_vlan else None
        yield build_ethernet(vlan_id, src_ip, dst_ip, protocol, src_port, dst_port, packet_id)


def main():
    parser = argparse.ArgumentParser(description="Generate Ethernet PCAP from SPI_rule workbook")
    parser.add_argument("--xlsx", default="SPI_DPI_rule.xlsx")
    parser.add_argument("--output", default="generated/spi_benchmark.pcap")
    parser.add_argument("--flows", type=int, default=100000)
    parser.add_argument("--packets", type=int, default=200000)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--no-vlan", action="store_true")
    args = parser.parse_args()

    if args.flows <= 0 or args.packets <= 0:
        raise SystemExit("--flows and --packets must be positive")
    rules = load_spi_rules_from_xlsx(args.xlsx)
    packets = generate_packets(rules, args.flows, args.packets, not args.no_vlan, args.seed)
    write_pcap(Path(args.output), packets)
    print(f"wrote {args.output}: packets={args.packets} flows={args.flows} rules={len(rules)}")


if __name__ == "__main__":
    main()
