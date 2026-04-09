#!/usr/bin/env python3
"""
Multi-node modem reliability test over SSH.

This script connects to multiple hosts over SSH, opens each host's modem serial
port through a lightweight remote agent, imports a configuration blob, and then
runs repeated TX/RX checks where each node transmits in turn.

It is designed for 4 nodes by default:
  node-1.local, node-2.local, node-3.local, node-4.local
"""

from __future__ import annotations

import argparse
import base64
import json
import queue
import re
import shlex
import subprocess
import sys
import threading
import time
import uuid
from collections import OrderedDict, deque
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional


DEFAULT_NODE_HOSTS = [
    "node-1.local",
    "node-2.local",
    "node-3.local",
    "node-4.local",
]


REMOTE_AGENT_CODE = r'''
import argparse
import json
import re
import sys
import threading
import time
from collections import deque

try:
    import serial
except Exception as exc:
    sys.stderr.write(f"remote agent: pyserial import failed: {exc}\n")
    sys.stderr.flush()
    raise


class LineStore:
    def __init__(self, max_lines):
        self._max_lines = max_lines
        self._entries = deque(maxlen=max_lines)
        self._next_seq = 1
        self._pending = ""
        self._cv = threading.Condition()

    def _append_line_locked(self, text):
        line = text.strip()
        if not line:
            return
        seq = self._next_seq
        self._next_seq += 1
        self._entries.append((seq, time.time(), line))
        self._cv.notify_all()

    def feed_text(self, text):
        if not text:
            return
        with self._cv:
            self._pending += text
            parts = re.split(r"\r\n|\n|\r", self._pending)
            self._pending = parts.pop() if parts else ""
            for part in parts:
                self._append_line_locked(part)

    def mark(self):
        with self._cv:
            return self._next_seq - 1

    def clear(self):
        with self._cv:
            self._entries.clear()
            self._pending = ""
            self._cv.notify_all()

    def lines_since(self, start_seq):
        with self._cv:
            return [entry[2] for entry in self._entries if entry[0] > start_seq]

    def collect_until_idle(self, start_seq, timeout_s, settle_s):
        deadline = time.monotonic() + max(0.01, timeout_s)
        settle_s = max(0.01, settle_s)
        with self._cv:
            last_seq = self._next_seq - 1
            last_change = time.monotonic()
            while True:
                now = time.monotonic()
                if now >= deadline:
                    break

                current_seq = self._next_seq - 1
                if current_seq != last_seq:
                    last_seq = current_seq
                    last_change = now
                elif (now - last_change) >= settle_s:
                    break

                next_wait = min(deadline - now, settle_s)
                if next_wait <= 0:
                    break
                self._cv.wait(timeout=next_wait)

            return [entry[2] for entry in self._entries if entry[0] > start_seq]

    def wait_regex(self, pattern, start_seq, timeout_s):
        compiled = re.compile(pattern)
        deadline = time.monotonic() + max(0.01, timeout_s)
        with self._cv:
            while True:
                for seq, _ts, line in self._entries:
                    if seq <= start_seq:
                        continue
                    if compiled.search(line):
                        return {"matched": True, "line": line, "seq": seq}

                now = time.monotonic()
                if now >= deadline:
                    return {"matched": False, "line": None, "seq": None}

                self._cv.wait(timeout=deadline - now)

    def recent(self, count):
        with self._cv:
            if count <= 0:
                return []
            return [entry[2] for entry in list(self._entries)[-count:]]


def run_agent(args):
    ser = serial.Serial(port=args.device, baudrate=args.baud, timeout=args.read_timeout_s)
    lines = LineStore(max_lines=args.max_lines)
    stop_event = threading.Event()

    def reader_loop():
        while not stop_event.is_set():
            try:
                waiting = ser.in_waiting
                data = ser.read(waiting if waiting > 0 else 1)
            except Exception as exc:
                sys.stderr.write(f"remote agent: serial read failed: {exc}\n")
                sys.stderr.flush()
                return

            if not data:
                continue

            text = data.decode("utf-8", errors="replace")
            lines.feed_text(text)

    reader = threading.Thread(target=reader_loop, daemon=True)
    reader.start()

    def respond(payload):
        sys.stdout.write(json.dumps(payload, separators=(",", ":")) + "\n")
        sys.stdout.flush()

    for raw in sys.stdin:
        raw = raw.strip()
        if not raw:
            continue

        req_id = None
        try:
            req = json.loads(raw)
            req_id = req.get("id")
            op = req.get("op")

            if op == "ping":
                respond({"id": req_id, "ok": True, "pong": True})
                continue

            if op == "mark":
                respond({"id": req_id, "ok": True, "mark_seq": lines.mark()})
                continue

            if op == "clear":
                try:
                    ser.reset_input_buffer()
                except Exception:
                    pass
                lines.clear()
                respond({"id": req_id, "ok": True})
                continue

            if op == "recent":
                count = int(req.get("count", 20))
                respond({"id": req_id, "ok": True, "lines": lines.recent(count)})
                continue

            if op == "send_cmd":
                command = req.get("command")
                if not isinstance(command, str) or not command:
                    respond({"id": req_id, "ok": False, "error": "command is required"})
                    continue

                timeout_s = float(req.get("timeout_s", 1.5))
                settle_s = float(req.get("settle_s", 0.2))
                start_seq = lines.mark()

                ser.write((command + "\r\n").encode("utf-8", errors="replace"))
                ser.flush()

                captured = lines.collect_until_idle(start_seq, timeout_s=timeout_s, settle_s=settle_s)
                respond({"id": req_id, "ok": True, "lines": captured})
                continue

            if op == "wait_regex":
                pattern = req.get("pattern")
                if not isinstance(pattern, str) or not pattern:
                    respond({"id": req_id, "ok": False, "error": "pattern is required"})
                    continue

                start_seq = int(req.get("start_seq", 0))
                timeout_s = float(req.get("timeout_s", 10.0))
                result = lines.wait_regex(pattern, start_seq=start_seq, timeout_s=timeout_s)
                respond({"id": req_id, "ok": True, **result})
                continue

            if op == "close":
                respond({"id": req_id, "ok": True})
                stop_event.set()
                break

            respond({"id": req_id, "ok": False, "error": f"unknown op: {op}"})

        except Exception as exc:
            respond({"id": req_id, "ok": False, "error": str(exc)})

    stop_event.set()
    try:
        ser.close()
    except Exception:
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", required=True)
    parser.add_argument("--baud", type=int, required=True)
    parser.add_argument("--read-timeout-s", type=float, default=0.05)
    parser.add_argument("--max-lines", type=int, default=20000)
    args = parser.parse_args()
    run_agent(args)


if __name__ == "__main__":
    main()
'''


@dataclass
class NodeSpec:
    name: str
    host: str
    device: str
    baud: int
    ssh_user: Optional[str] = None
    ssh_port: int = 22
    ssh_options: list[str] = field(default_factory=list)

    @property
    def ssh_target(self) -> str:
        if self.ssh_user:
            return f"{self.ssh_user}@{self.host}"
        return self.host


class SshSerialBridge:
    def __init__(self, node: NodeSpec, read_timeout_s: float, max_lines: int = 20000) -> None:
        self.node = node
        self.read_timeout_s = read_timeout_s
        self.max_lines = max_lines
        self._proc: Optional[subprocess.Popen[str]] = None
        self._stdout_queue: queue.Queue[dict[str, Any]] = queue.Queue()
        self._stderr_lines: deque[str] = deque(maxlen=200)
        self._stdout_thread: Optional[threading.Thread] = None
        self._stderr_thread: Optional[threading.Thread] = None
        self._next_id = 1
        self._request_lock = threading.Lock()

    def connect(self) -> None:
        code_b64 = base64.b64encode(REMOTE_AGENT_CODE.encode("utf-8")).decode("ascii")
        bootstrap = (
            "import base64;"
            f"exec(base64.b64decode({code_b64!r}).decode('utf-8'))"
        )

        remote_parts = [
            "python3",
            "-u",
            "-c",
            bootstrap,
            "--",
            "--device",
            self.node.device,
            "--baud",
            str(self.node.baud),
            "--read-timeout-s",
            str(self.read_timeout_s),
            "--max-lines",
            str(self.max_lines),
        ]
        remote_command = " ".join(shlex.quote(part) for part in remote_parts)

        cmd = [
            "ssh",
            "-o",
            "BatchMode=yes",
            "-o",
            "StrictHostKeyChecking=accept-new",
            "-p",
            str(self.node.ssh_port),
        ]
        for option in self.node.ssh_options:
            cmd.extend(["-o", option])
        cmd.extend([self.node.ssh_target, remote_command])

        self._proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )

        assert self._proc.stdout is not None
        assert self._proc.stderr is not None

        self._stdout_thread = threading.Thread(target=self._stdout_reader, daemon=True)
        self._stderr_thread = threading.Thread(target=self._stderr_reader, daemon=True)
        self._stdout_thread.start()
        self._stderr_thread.start()

        self.request("ping", rpc_timeout_s=8.0)

    def _stdout_reader(self) -> None:
        assert self._proc is not None
        assert self._proc.stdout is not None

        for line in self._proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                payload = json.loads(line)
            except json.JSONDecodeError:
                self._stderr_lines.append(f"non-json stdout: {line}")
                continue
            self._stdout_queue.put(payload)

    def _stderr_reader(self) -> None:
        assert self._proc is not None
        assert self._proc.stderr is not None

        for line in self._proc.stderr:
            line = line.strip()
            if line:
                self._stderr_lines.append(line)

    def request(self, op: str, rpc_timeout_s: float = 10.0, **kwargs: Any) -> dict[str, Any]:
        if self._proc is None or self._proc.stdin is None:
            raise RuntimeError(f"{self.node.name}: bridge is not connected")

        with self._request_lock:
            if self._proc.poll() is not None:
                raise RuntimeError(
                    f"{self.node.name}: ssh process exited with code {self._proc.returncode}; "
                    f"stderr={list(self._stderr_lines)}"
                )

            req_id = self._next_id
            self._next_id += 1

            request_payload = {"id": req_id, "op": op}
            request_payload.update(kwargs)

            self._proc.stdin.write(json.dumps(request_payload, separators=(",", ":")) + "\n")
            self._proc.stdin.flush()

            deadline = time.monotonic() + rpc_timeout_s
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(f"{self.node.name}: timed out waiting for op={op}")

                try:
                    response = self._stdout_queue.get(timeout=remaining)
                except queue.Empty as exc:
                    raise TimeoutError(f"{self.node.name}: timed out waiting for op={op}") from exc

                if response.get("id") != req_id:
                    continue

                if not response.get("ok", False):
                    raise RuntimeError(
                        f"{self.node.name}: remote op {op} failed: {response.get('error')}"
                    )
                return response

    def send_command(self, command: str, timeout_s: float, settle_s: float) -> list[str]:
        response = self.request(
            "send_cmd",
            rpc_timeout_s=timeout_s + 4.0,
            command=command,
            timeout_s=timeout_s,
            settle_s=settle_s,
        )
        return [str(line) for line in response.get("lines", [])]

    def mark(self) -> int:
        response = self.request("mark", rpc_timeout_s=5.0)
        return int(response["mark_seq"])

    def wait_regex(self, pattern: str, start_seq: int, timeout_s: float) -> dict[str, Any]:
        return self.request(
            "wait_regex",
            rpc_timeout_s=timeout_s + 2.0,
            pattern=pattern,
            start_seq=start_seq,
            timeout_s=timeout_s,
        )

    def recent_lines(self, count: int = 20) -> list[str]:
        response = self.request("recent", rpc_timeout_s=5.0, count=count)
        return [str(line) for line in response.get("lines", [])]

    def clear(self) -> None:
        self.request("clear", rpc_timeout_s=5.0)

    def close(self) -> None:
        if self._proc is None:
            return

        try:
            self.request("close", rpc_timeout_s=2.0)
        except Exception:
            pass

        if self._proc.poll() is None:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                self._proc.kill()


def normalize_config_blob_markers(blob: str) -> str:
    out = blob.strip().strip('"').strip("'")
    out = out.replace("\r", "").replace("\n", "")
    replacements = {
        r"\bSTARTALL\b": "START_ALL",
        r"\bENDALL\b": "END_ALL",
        r"\bSTARTSOME\b": "START_SOME",
        r"\bENDSOME\b": "END_SOME",
    }
    for pattern, replacement in replacements.items():
        out = re.sub(pattern, replacement, out, flags=re.IGNORECASE)
    return out


def escape_comm_quoted_argument(text: str) -> str:
    escaped = text.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def parse_config_blob(blob: str) -> tuple[str, str, OrderedDict[int, str]]:
    normalized = normalize_config_blob_markers(blob)

    markers: list[tuple[str, str]] = [
        ("START_ALL", "END_ALL"),
        ("START_SOME", "END_SOME"),
    ]

    selected: Optional[tuple[str, str]] = None
    for start_marker, end_marker in markers:
        if start_marker in normalized and end_marker in normalized:
            selected = (start_marker, end_marker)
            break

    if selected is None:
        raise ValueError("Config blob must include START_ALL/END_ALL or START_SOME/END_SOME")

    start_marker, end_marker = selected
    start_index = normalized.find(start_marker)
    end_index = normalized.find(end_marker, start_index + len(start_marker))
    if end_index < 0:
        raise ValueError(f"Config blob missing end marker: {end_marker}")

    content = normalized[start_index + len(start_marker):end_index]
    params: OrderedDict[int, str] = OrderedDict()

    for token in content.split(","):
        token = token.strip()
        if not token:
            continue

        if "-" not in token:
            continue

        left, right = token.split("-", 1)
        left = left.strip()
        right = right.strip()
        if not left:
            continue

        try:
            param_id = int(left, 10)
        except ValueError:
            continue

        params[param_id] = right

    return start_marker, end_marker, params


def render_config_blob(start_marker: str, end_marker: str, params: OrderedDict[int, str]) -> str:
    body = ",".join(f"{param_id}-{value}" for param_id, value in params.items())
    if body:
        return f"{start_marker},{body},{end_marker}"
    return f"{start_marker},{end_marker}"


def load_config_blob(config_blob: Optional[str], config_file: Optional[str]) -> str:
    if config_blob and config_file:
        raise ValueError("Provide either --config-blob or --config-file, not both")
    if not config_blob and not config_file:
        raise ValueError("Provide one of --config-blob or --config-file")

    if config_file:
        return Path(config_file).read_text(encoding="utf-8")
    assert config_blob is not None
    return config_blob


def parse_node_specs(
    nodes_csv: str,
    default_device: str,
    default_baud: int,
    ssh_user: Optional[str],
    ssh_port: int,
    ssh_options: list[str],
    node_config_file: Optional[str],
) -> list[NodeSpec]:
    if node_config_file:
        raw = json.loads(Path(node_config_file).read_text(encoding="utf-8"))
        if not isinstance(raw, list) or not raw:
            raise ValueError("--node-config-file must contain a non-empty JSON list")

        nodes: list[NodeSpec] = []
        for index, item in enumerate(raw, start=1):
            if not isinstance(item, dict):
                raise ValueError("Each node entry must be a JSON object")
            host = str(item.get("host", "")).strip()
            if not host:
                raise ValueError(f"Node entry #{index} is missing host")

            nodes.append(
                NodeSpec(
                    name=str(item.get("name", host)),
                    host=host,
                    device=str(item.get("device", default_device)),
                    baud=int(item.get("baud", default_baud)),
                    ssh_user=item.get("ssh_user", ssh_user),
                    ssh_port=int(item.get("ssh_port", ssh_port)),
                    ssh_options=list(item.get("ssh_options", ssh_options)),
                )
            )

        return nodes

    hosts = [host.strip() for host in nodes_csv.split(",") if host.strip()]
    if not hosts:
        raise ValueError("No nodes provided")

    return [
        NodeSpec(
            name=host,
            host=host,
            device=default_device,
            baud=default_baud,
            ssh_user=ssh_user,
            ssh_port=ssh_port,
            ssh_options=ssh_options,
        )
        for host in hosts
    ]


class ReliabilityHarness:
    def __init__(
        self,
        nodes: list[NodeSpec],
        command_timeout_s: float,
        settle_s: float,
        receive_timeout_s: float,
        read_timeout_s: float,
        tx_route: str,
        include_sender_receive: bool,
    ) -> None:
        self.nodes = nodes
        self.command_timeout_s = command_timeout_s
        self.settle_s = settle_s
        self.receive_timeout_s = receive_timeout_s
        self.read_timeout_s = read_timeout_s
        self.tx_route = tx_route
        self.include_sender_receive = include_sender_receive
        self.bridges: dict[str, SshSerialBridge] = {}

    def connect(self) -> None:
        for node in self.nodes:
            bridge = SshSerialBridge(node=node, read_timeout_s=self.read_timeout_s)
            bridge.connect()
            self.bridges[node.name] = bridge
            print(f"[connect] {node.name} via {node.ssh_target} device={node.device} baud={node.baud}")

    def close(self) -> None:
        for bridge in self.bridges.values():
            bridge.close()

    def _send_and_expect(self, node_name: str, command: str, regex: str) -> list[str]:
        bridge = self.bridges[node_name]
        lines = bridge.send_command(command, timeout_s=self.command_timeout_s, settle_s=self.settle_s)
        if not any(re.search(regex, line) for line in lines):
            raise RuntimeError(
                f"{node_name}: command failed expectation: {command}; "
                f"expected={regex}; lines={lines}"
            )
        return lines

    def configure_all(self, config_blob: str) -> None:
        normalized_blob = normalize_config_blob_markers(config_blob)
        import_cmd = f":importcfg {escape_comm_quoted_argument(normalized_blob)}"

        for node in self.nodes:
            bridge = self.bridges[node.name]
            bridge.clear()

            self._send_and_expect(node.name, ":tag on", r"\[STATUS\]\s+OK :tag on")
            self._send_and_expect(node.name, ":prompt off", r"\[STATUS\]\s+OK :prompt off")
            self._send_and_expect(node.name, ":print off", r"\[STATUS\]\s+OK :print off")
            self._send_and_expect(node.name, ":rxsub on", r"\[STATUS\]\s+OK :rxsub on")

            lines = bridge.send_command(import_cmd, timeout_s=max(self.command_timeout_s, 4.0), settle_s=self.settle_s)
            if not any(re.search(r"\[STATUS\]\s+OK :importcfg", line) for line in lines):
                raise RuntimeError(f"{node.name}: config import failed; lines={lines}")

            self._send_and_expect(node.name, ":status", r"\bmode=(hostmac|local)\b")
            print(f"[configure] {node.name} imported config and enabled rxsub")

    def run_reliability(
        self,
        messages_per_node: int,
        inter_send_delay_s: float,
        payload_prefix: str,
    ) -> dict[str, Any]:
        tx_route = self.tx_route.strip().lower()
        if tx_route not in {"transducer", "feedback"}:
            raise ValueError("tx_route must be transducer or feedback")

        transmission_records: list[dict[str, Any]] = []
        total_expected_deliveries = 0
        successful_deliveries = 0
        total_transmissions = len(self.nodes) * messages_per_node
        full_delivery_transmissions = 0

        tx_index = 0
        for sender in self.nodes:
            for iteration in range(1, messages_per_node + 1):
                tx_index += 1

                payload_text = (
                    f"{payload_prefix}|src={sender.name}|iter={iteration}|nonce={uuid.uuid4().hex[:12]}"
                )
                payload_b64 = base64.b64encode(payload_text.encode("utf-8")).decode("ascii")

                wait_targets = [
                    node for node in self.nodes
                    if self.include_sender_receive or node.name != sender.name
                ]

                marks: dict[str, int] = {}
                for target in wait_targets:
                    marks[target.name] = self.bridges[target.name].mark()

                tx_cmd = f":tx route={tx_route} payload={payload_b64}"
                ack_lines = self.bridges[sender.name].send_command(
                    tx_cmd,
                    timeout_s=self.command_timeout_s,
                    settle_s=self.settle_s,
                )
                ack_ok = any(
                    re.search(rf"\[STATUS\]\s+OK :tx route={re.escape(tx_route)}", line)
                    for line in ack_lines
                )

                rx_pattern = (
                    rf"\[MSG_RX\]\s+EVENT\s+"
                    rf"(?=.*\berror=0\b)(?=.*\bpayload_b64={re.escape(payload_b64)}(?:\s|$)).*"
                )

                receiver_results: list[dict[str, Any]] = []
                delivered_to_all = ack_ok

                for receiver in wait_targets:
                    wait_result = self.bridges[receiver.name].wait_regex(
                        pattern=rx_pattern,
                        start_seq=marks[receiver.name],
                        timeout_s=self.receive_timeout_s,
                    )

                    received = bool(wait_result.get("matched", False))
                    matched_line = wait_result.get("line")
                    if not received:
                        delivered_to_all = False

                    receiver_results.append(
                        {
                            "receiver": receiver.name,
                            "received": received,
                            "matched_line": matched_line,
                        }
                    )

                expected_for_tx = len(wait_targets)
                success_for_tx = sum(1 for item in receiver_results if item["received"])
                total_expected_deliveries += expected_for_tx
                successful_deliveries += success_for_tx
                if delivered_to_all and expected_for_tx > 0:
                    full_delivery_transmissions += 1

                print(
                    f"[tx {tx_index}/{total_transmissions}] sender={sender.name} iter={iteration} "
                    f"ack={'ok' if ack_ok else 'fail'} delivered={success_for_tx}/{expected_for_tx}"
                )

                transmission_records.append(
                    {
                        "sender": sender.name,
                        "iteration": iteration,
                        "tx_command": tx_cmd,
                        "tx_ack_ok": ack_ok,
                        "tx_ack_lines": ack_lines,
                        "payload_plain": payload_text,
                        "payload_b64": payload_b64,
                        "receivers": receiver_results,
                    }
                )

                if inter_send_delay_s > 0:
                    time.sleep(inter_send_delay_s)

        per_sender: dict[str, dict[str, Any]] = {}
        for node in self.nodes:
            sender_records = [r for r in transmission_records if r["sender"] == node.name]
            sender_expected = sum(len(r["receivers"]) for r in sender_records)
            sender_success = sum(
                sum(1 for recv in r["receivers"] if recv["received"]) for r in sender_records
            )
            sender_full = sum(
                1 for r in sender_records if r["tx_ack_ok"] and all(v["received"] for v in r["receivers"])
            )
            per_sender[node.name] = {
                "messages_sent": len(sender_records),
                "expected_deliveries": sender_expected,
                "successful_deliveries": sender_success,
                "delivery_reliability": (
                    float(sender_success) / float(sender_expected) if sender_expected else 0.0
                ),
                "full_delivery_messages": sender_full,
            }

        return {
            "timestamp_utc": datetime.now(timezone.utc).isoformat(),
            "nodes": [node.name for node in self.nodes],
            "messages_per_node": messages_per_node,
            "tx_route": tx_route,
            "include_sender_receive": self.include_sender_receive,
            "total_transmissions": total_transmissions,
            "expected_deliveries": total_expected_deliveries,
            "successful_deliveries": successful_deliveries,
            "delivery_reliability": (
                float(successful_deliveries) / float(total_expected_deliveries)
                if total_expected_deliveries
                else 0.0
            ),
            "full_delivery_transmissions": full_delivery_transmissions,
            "full_delivery_rate": (
                float(full_delivery_transmissions) / float(total_transmissions)
                if total_transmissions
                else 0.0
            ),
            "per_sender": per_sender,
            "transmissions": transmission_records,
        }


def run_reliability_campaign(
    node_specs: list[NodeSpec],
    config_blob: str,
    command_timeout_s: float,
    settle_s: float,
    receive_timeout_s: float,
    read_timeout_s: float,
    tx_route: str,
    messages_per_node: int,
    inter_send_delay_s: float,
    payload_prefix: str,
    include_sender_receive: bool,
) -> dict[str, Any]:
    harness = ReliabilityHarness(
        nodes=node_specs,
        command_timeout_s=command_timeout_s,
        settle_s=settle_s,
        receive_timeout_s=receive_timeout_s,
        read_timeout_s=read_timeout_s,
        tx_route=tx_route,
        include_sender_receive=include_sender_receive,
    )

    try:
        harness.connect()
        harness.configure_all(config_blob=config_blob)
        return harness.run_reliability(
            messages_per_node=messages_per_node,
            inter_send_delay_s=inter_send_delay_s,
            payload_prefix=payload_prefix,
        )
    finally:
        harness.close()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Run multi-node modem reliability testing over SSH using :importcfg and :tx/:rxsub commands."
        )
    )

    parser.add_argument(
        "--nodes",
        default=",".join(DEFAULT_NODE_HOSTS),
        help="Comma-separated hostnames (default: node-1.local,node-2.local,node-3.local,node-4.local)",
    )
    parser.add_argument(
        "--node-config-file",
        help="Optional JSON list of per-node SSH/serial settings",
    )
    parser.add_argument("--device", default="/dev/ttyACM0", help="Default serial device on each node")
    parser.add_argument("--baud", type=int, default=115200, help="Default serial baud on each node")
    parser.add_argument("--ssh-user", help="Optional SSH username")
    parser.add_argument("--ssh-port", type=int, default=22, help="SSH port (default: 22)")
    parser.add_argument(
        "--ssh-option",
        action="append",
        default=[],
        help="Additional ssh -o option (repeatable), example: ConnectTimeout=10",
    )

    parser.add_argument("--config-blob", help="Configuration blob text (START_ALL...END_ALL or STARTALL...ENDALL)")
    parser.add_argument("--config-file", help="Path to file containing the config blob")

    parser.add_argument("--tx-route", choices=["transducer", "feedback"], default="transducer")
    parser.add_argument("--messages-per-node", type=int, default=10)
    parser.add_argument("--include-sender-receive", action="store_true")
    parser.add_argument("--payload-prefix", default="RELIABILITY")

    parser.add_argument("--read-timeout-s", type=float, default=0.05)
    parser.add_argument("--command-timeout-s", type=float, default=2.0)
    parser.add_argument("--settle-s", type=float, default=0.25)
    parser.add_argument("--receive-timeout-s", type=float, default=20.0)
    parser.add_argument("--inter-send-delay-s", type=float, default=0.25)

    parser.add_argument("--summary-json", help="Optional output path for run summary JSON")
    parser.add_argument(
        "--min-delivery-reliability",
        type=float,
        default=0.0,
        help="Exit with failure if achieved reliability is below this threshold",
    )

    return parser


def main() -> int:
    args = build_parser().parse_args()

    try:
        node_specs = parse_node_specs(
            nodes_csv=args.nodes,
            default_device=args.device,
            default_baud=args.baud,
            ssh_user=args.ssh_user,
            ssh_port=args.ssh_port,
            ssh_options=args.ssh_option,
            node_config_file=args.node_config_file,
        )
        config_blob = load_config_blob(args.config_blob, args.config_file)

        summary = run_reliability_campaign(
            node_specs=node_specs,
            config_blob=config_blob,
            command_timeout_s=args.command_timeout_s,
            settle_s=args.settle_s,
            receive_timeout_s=args.receive_timeout_s,
            read_timeout_s=args.read_timeout_s,
            tx_route=args.tx_route,
            messages_per_node=args.messages_per_node,
            inter_send_delay_s=args.inter_send_delay_s,
            payload_prefix=args.payload_prefix,
            include_sender_receive=args.include_sender_receive,
        )
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 2

    reliability = summary["delivery_reliability"]
    print()
    print("=== Reliability Summary ===")
    print(f"Nodes: {', '.join(summary['nodes'])}")
    print(f"Messages per node: {summary['messages_per_node']}")
    print(f"TX route: {summary['tx_route']}")
    print(f"Expected deliveries: {summary['expected_deliveries']}")
    print(f"Successful deliveries: {summary['successful_deliveries']}")
    print(f"Delivery reliability: {reliability:.4f}")
    print(f"Full-delivery transmissions: {summary['full_delivery_transmissions']} / {summary['total_transmissions']}")
    print(f"Full-delivery rate: {summary['full_delivery_rate']:.4f}")

    if args.summary_json:
        Path(args.summary_json).write_text(json.dumps(summary, indent=2), encoding="utf-8")
        print(f"Summary JSON written to {args.summary_json}")

    if reliability < args.min_delivery_reliability:
        print(
            f"Reliability below threshold: {reliability:.4f} < {args.min_delivery_reliability:.4f}",
            file=sys.stderr,
        )
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
