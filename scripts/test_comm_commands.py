#!/usr/bin/env python3
"""
COMM command test harness for OpenAquatix firmware.

This script validates the machine-command surface over USB/UART while
explicitly avoiding command invocations that use the transducer route.

By default it tests:
- parser behavior and help text
- session/control commands (:tag, :prompt, :print, :mode)
- diagnostics (:time, :status, :telemetry, :telemetrysub, :errorlog, :config)
- subscriptions (:rxsub, :sense)
- optional traffic commands only through route=feedback (:tx, :range, :txat)

Requirements:
- pyserial (pip install pyserial)
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from dataclasses import dataclass, field
from typing import Iterable, Optional

try:
    import serial
except ImportError as exc:
    print("pyserial is required. Install it with: pip install pyserial", file=sys.stderr)
    raise SystemExit(2) from exc


LINE_ENDING = "\r\n"
TIME_LINE_RE = re.compile(r"OK :time tick_ms=(\d+) cyccnt=(\d+)")
TXAT_OK_RE = re.compile(r"OK :txat request_id=(\d+) protocol=(custom|janus) tx_cyccnt=(\d+)")


@dataclass
class CommandResult:
    command: str
    raw_text: str
    lines: list[str]


@dataclass
class TestRecord:
    name: str
    passed: bool
    detail: str = ""
    command: Optional[str] = None
    response_lines: list[str] = field(default_factory=list)


class SerialSession:
    def __init__(self, port: str, baud: int, read_timeout_s: float) -> None:
        self._serial = serial.Serial(port=port, baudrate=baud, timeout=read_timeout_s)

    def close(self) -> None:
        self._serial.close()

    def drain_input(self, duration_s: float = 0.25) -> None:
        end_time = time.monotonic() + duration_s
        while time.monotonic() < end_time:
            waiting = self._serial.in_waiting
            if waiting > 0:
                self._serial.read(waiting)
                end_time = time.monotonic() + 0.05
            else:
                time.sleep(0.005)

    def send_command(self, command: str, timeout_s: float, settle_s: float) -> CommandResult:
        self.drain_input()

        payload = (command + LINE_ENDING).encode("utf-8", errors="replace")
        self._serial.write(payload)
        self._serial.flush()

        start = time.monotonic()
        last_receive_time: Optional[float] = None
        buffer = bytearray()

        while True:
            now = time.monotonic()
            if now - start >= timeout_s:
                break

            waiting = self._serial.in_waiting
            if waiting > 0:
                chunk = self._serial.read(waiting)
                if chunk:
                    buffer.extend(chunk)
                    last_receive_time = now
                continue

            if last_receive_time is not None and (now - last_receive_time) >= settle_s:
                break

            time.sleep(0.005)

        raw_text = buffer.decode("utf-8", errors="replace")
        lines: list[str] = []
        for raw_line in re.split(r"\r\n|\n|\r", raw_text):
            line = raw_line.strip()
            if line:
                lines.append(line)

        return CommandResult(command=command, raw_text=raw_text, lines=lines)


class SuiteRunner:
    def __init__(
        self,
        session: SerialSession,
        timeout_s: float,
        settle_s: float,
        verbose: bool,
    ) -> None:
        self._session = session
        self._timeout_s = timeout_s
        self._settle_s = settle_s
        self._verbose = verbose
        self.records: list[TestRecord] = []

    @staticmethod
    def _first_match(lines: Iterable[str], pattern: str) -> Optional[str]:
        matcher = re.compile(pattern)
        for line in lines:
            if matcher.search(line):
                return line
        return None

    @staticmethod
    def _assert_non_transducer(command: str) -> None:
        lowered = command.strip().lower()
        if "route=transducer" in lowered:
            raise ValueError(f"Transducer route is not allowed in this harness: {command}")
        if lowered == ":range":
            raise ValueError("Default :range uses transducer route; use :range route=feedback")

    def _record(self, test: TestRecord) -> None:
        self.records.append(test)

        status = "PASS" if test.passed else "FAIL"
        detail_text = f" ({test.detail})" if test.detail else ""
        print(f"[{status}] {test.name}{detail_text}")

        if self._verbose:
            if test.command:
                print(f"  command: {test.command}")
            if test.response_lines:
                print("  response:")
                for line in test.response_lines:
                    print(f"    {line}")

    def run_command_test(
        self,
        name: str,
        command: str,
        expected_patterns: Iterable[str],
        forbidden_patterns: Iterable[str] = (),
        timeout_s: Optional[float] = None,
    ) -> tuple[bool, CommandResult]:
        self._assert_non_transducer(command)

        result = self._session.send_command(
            command,
            timeout_s if timeout_s is not None else self._timeout_s,
            self._settle_s,
        )

        missing = [p for p in expected_patterns if self._first_match(result.lines, p) is None]
        unexpected = [p for p in forbidden_patterns if self._first_match(result.lines, p) is not None]

        passed = not missing and not unexpected
        detail_parts: list[str] = []
        if missing:
            detail_parts.append("missing pattern(s): " + ", ".join(missing))
        if unexpected:
            detail_parts.append("forbidden pattern(s) seen: " + ", ".join(unexpected))
        if not result.lines:
            detail_parts.append("no response captured")

        self._record(
            TestRecord(
                name=name,
                passed=passed,
                detail="; ".join(detail_parts),
                command=command,
                response_lines=result.lines,
            )
        )
        return passed, result

    def run_condition_test(self, name: str, passed: bool, detail: str = "") -> None:
        self._record(TestRecord(name=name, passed=passed, detail=detail))

    def extract_time(self) -> tuple[bool, Optional[int], Optional[int]]:
        ok, result = self.run_command_test(
            name="Read device time",
            command=":time",
            expected_patterns=[r"\[STATUS\]\s+OK :time tick_ms=\d+ cyccnt=\d+"],
        )
        if not ok:
            return False, None, None

        line = self._first_match(result.lines, TIME_LINE_RE.pattern)
        if line is None:
            self.run_condition_test(
                name="Parse :time response",
                passed=False,
                detail="Expected tick_ms/cyccnt tokens not found",
            )
            return False, None, None

        match = TIME_LINE_RE.search(line)
        if match is None:
            self.run_condition_test(
                name="Parse :time response",
                passed=False,
                detail="Regex match failed for :time output",
            )
            return False, None, None

        tick_ms = int(match.group(1))
        cyccnt = int(match.group(2))
        self.run_condition_test(
            name="Parse :time response",
            passed=True,
            detail=f"tick_ms={tick_ms} cyccnt={cyccnt}",
        )
        return True, tick_ms, cyccnt

    @property
    def pass_count(self) -> int:
        return sum(1 for r in self.records if r.passed)

    @property
    def fail_count(self) -> int:
        return sum(1 for r in self.records if not r.passed)


def parse_int(text: str) -> int:
    return int(text, 0)


def run_suite(runner: SuiteRunner, args: argparse.Namespace) -> None:
    # Session setup and parser behavior.
    runner.run_command_test("Enable tagged output", ":tag on", [r"\[STATUS\]\s+OK :tag on"])
    runner.run_command_test("Disable prompts", ":prompt off", [r"\[STATUS\]\s+OK :prompt off"])
    runner.run_command_test(
        "Unknown command handling",
        ":doesnotexist",
        [r"\[ERROR\]\s+Unknown command: :doesnotexist"],
    )
    runner.run_command_test(
        "Case-insensitive parser",
        "  :STATUS",
        [r"\[STATUS\]\s+OK :status\s"],
    )
    runner.run_command_test(
        "Help command summary",
        ":help",
        [r"\[STATUS\]\s+Available commands:", r"\[STATUS\]\s+:status\s+-\s+"],
    )
    runner.run_command_test(
        "Help command detail",
        ":help tx",
        [r"\[STATUS\]\s+Command: :tx", r"\[STATUS\]\s+Usage: :tx route=<transducer\|feedback> payload=<base64>"],
    )
    runner.run_command_test(
        "Bad quoting returns usage",
        ':help "unterminated',
        [r"\[ERROR\]\s+Usage: :help \[command\]"],
    )

    # Session behavior and forced machine lines.
    runner.run_command_test("Disable tag mode", ":tag off", [r"\[STATUS\]\s+OK :tag off"])
    runner.run_command_test(
        "Status still machine-tagged when :tag off",
        ":status",
        [r"\[STATUS\]\s+OK :status\s+"],
    )
    runner.run_command_test("Re-enable tag mode", ":tag on", [r"\[STATUS\]\s+OK :tag on"])

    # Core toggles.
    runner.run_command_test("Enable prompts", ":prompt on", [r"\[STATUS\]\s+OK :prompt on"])
    runner.run_command_test("Disable prompts again", ":prompt off", [r"\[STATUS\]\s+OK :prompt off"])
    runner.run_command_test("Enable RX print", ":print on", [r"\[STATUS\]\s+OK :print on"])
    runner.run_command_test("Disable RX print", ":print off", [r"\[STATUS\]\s+OK :print off"])

    # MAC mode and status.
    runner.run_command_test("Enable host MAC mode", ":mode hostmac on", [r"\[STATUS\]\s+OK :mode hostmac on"])
    runner.run_command_test("Status shows hostmac mode", ":status", [r"\bmode=hostmac\b"])
    runner.run_command_test(
        "Reject invalid mode arg",
        ":mode hostmac maybe",
        [r"\[ERROR\]\s+Usage: :mode hostmac on\|off"],
    )
    runner.run_command_test("Disable host MAC mode", ":mode hostmac off", [r"\[STATUS\]\s+OK :mode hostmac off"])
    runner.run_command_test("Status shows local mode", ":status", [r"\bmode=local\b"])

    # Time monotonicity.
    first_ok, tick1, cyc1 = runner.extract_time()
    time.sleep(0.05)
    second_ok, tick2, cyc2 = runner.extract_time()
    if first_ok and second_ok and tick1 is not None and tick2 is not None and cyc1 is not None and cyc2 is not None:
        monotonic_ok = tick2 >= tick1
        cyccnt_changed = cyc2 != cyc1
        runner.run_condition_test(
            "Validate :time monotonicity",
            monotonic_ok and cyccnt_changed,
            detail=f"tick1={tick1} tick2={tick2} cyccnt1={cyc1} cyccnt2={cyc2}",
        )

    # Subscriptions.
    runner.run_command_test("Enable RX subscription", ":rxsub on", [r"\[STATUS\]\s+OK :rxsub on"])
    runner.run_command_test("Disable RX subscription", ":rxsub off", [r"\[STATUS\]\s+OK :rxsub off"])
    runner.run_command_test("Enable sensing subscription", ":sense on", [r"\[STATUS\]\s+OK :sense on"])
    runner.run_command_test("Disable sensing subscription", ":sense off", [r"\[STATUS\]\s+OK :sense off"])

    # Telemetry snapshot and stream controls.
    runner.run_command_test(
        "Telemetry snapshot default group",
        ":telemetry",
        [
            r"\[STATUS\]\s+OK :telemetry group=all",
            r"\btemp_ready=(yes|no)\b",
            r"\bpower_ready=(yes|no)\b",
            r"\belectrical_ready=(yes|no)\b",
            r"\benv_ready=(yes|no)\b",
        ],
    )
    runner.run_command_test("Telemetry temp group", ":telemetry temp", [r"\[STATUS\]\s+OK :telemetry group=temp\b"])
    runner.run_command_test("Telemetry power group", ":telemetry power", [r"\[STATUS\]\s+OK :telemetry group=power\b"])
    runner.run_command_test("Telemetry electrical group", ":telemetry electrical", [r"\[STATUS\]\s+OK :telemetry group=electrical\b"])
    runner.run_command_test("Telemetry env group", ":telemetry env", [r"\[STATUS\]\s+OK :telemetry group=env\b"])
    runner.run_command_test(
        "Reject invalid telemetry group",
        ":telemetry invalid",
        [r"\[ERROR\]\s+Usage: :telemetry \[all\|temp\|power\|electrical\|env\]"],
    )

    runner.run_command_test(
        "Enable telemetry stream defaults",
        ":telemetrysub on",
        [r"\[STATUS\]\s+OK :telemetrysub on group=all period_ms=1000"],
    )
    runner.run_command_test(
        "Status shows telemetry stream enabled",
        ":status",
        [r"\btelemetrysub=on\b", r"\btelemetry_group=all\b", r"\btelemetry_period_ms=1000\b"],
    )
    runner.run_command_test(
        "Reconfigure telemetry stream",
        ":telemetrysub on power period_ms=250",
        [r"\[STATUS\]\s+OK :telemetrysub on group=power period_ms=250"],
    )
    runner.run_command_test(
        "Reject out-of-range telemetry period",
        ":telemetrysub on power period_ms=50",
        [r"\[ERROR\]\s+Usage: :telemetrysub on\|off \[all\|temp\|power\|electrical\|env\] \[period_ms=<u32>\]"],
    )
    runner.run_command_test(
        "Reject extra arg on telemetrysub off",
        ":telemetrysub off power",
        [r"\[ERROR\]\s+Usage: :telemetrysub on\|off \[all\|temp\|power\|electrical\|env\] \[period_ms=<u32>\]"],
    )
    runner.run_command_test("Disable telemetry stream", ":telemetrysub off", [r"\[STATUS\]\s+OK :telemetrysub off"])
    runner.run_command_test(
        "Status shows telemetry stream disabled",
        ":status",
        [r"\btelemetrysub=off\b", r"\btelemetry_group=none\b", r"\btelemetry_period_ms=0\b"],
    )

    # Error log + config command.
    runner.run_command_test(
        "Read error log summary",
        ":errorlog",
        [r"\[STATUS\]\s+OK :errorlog count=\d+ current_tick_ms=\d+ current_reset_count=\d+"],
        timeout_s=max(args.command_timeout_s, 2.0),
    )
    runner.run_command_test(
        "Read baud config parameter",
        ":config get baud",
        [r"\[STATUS\]\s+OK :config get baud value=\d+(?:\.\d+)? units=bps"],
    )

    # Cancel tests do not require transducer traffic.
    runner.run_command_test(
        "Reject invalid cancel_tx id",
        ":cancel_tx not_a_number",
        [r"\[ERROR\]\s+Invalid request id:"],
    )
    runner.run_command_test(
        "Reject unknown cancel_tx id",
        ":cancel_tx 999999",
        [r"\[ERROR\]\s+No pending scheduled transmission with id=999999"],
    )

    if args.run_importcfg_negative:
        runner.run_command_test(
            "Malformed importcfg is rejected",
            ":importcfg BAD_BLOB",
            [r"\[ERROR\]\s+CFG_IMPORT_"],
            timeout_s=max(args.command_timeout_s, 2.0),
        )

    if args.skip_feedback_traffic:
        return

    # Feedback-only traffic command tests.
    runner.run_command_test(
        "Reject invalid tx route",
        ":tx route=invalid payload=QQ==",
        [r"\[ERROR\]\s+Invalid route: invalid"],
    )
    runner.run_command_test(
        "Reject tx missing payload",
        ":tx route=feedback",
        [r"\[ERROR\]\s+Usage: :tx route=<transducer\|feedback> payload=<base64>"],
    )
    runner.run_command_test(
        "Immediate tx through feedback route",
        ":tx route=feedback payload=QQ==",
        [r"\[STATUS\]\s+OK :tx route=feedback"],
    )

    runner.run_command_test(
        "Reject invalid range route",
        ":range route=invalid",
        [r"\[ERROR\]\s+Usage: :range \[route=transducer\|feedback\]"],
    )
    runner.run_command_test(
        "Queue range through feedback route",
        ":range route=feedback",
        [r"\[STATUS\]\s+OK :range route=feedback"],
    )

    runner.run_command_test("Ensure local mode before txat gate test", ":mode hostmac off", [r"\[STATUS\]\s+OK :mode hostmac off"])
    runner.run_command_test(
        "txat requires host MAC mode",
        ":txat route=feedback protocol=custom payload=QQ== tx_cyccnt=1 type=bits",
        [r"\[ERROR\]\s+Host MAC mode must be enabled before using :txat"],
    )

    runner.run_command_test("Enable host MAC for txat", ":mode hostmac on", [r"\[STATUS\]\s+OK :mode hostmac on"])
    runner.run_command_test(
        "txat custom requires type",
        ":txat route=feedback protocol=custom payload=QQ== tx_cyccnt=123456",
        [r"\[ERROR\]\s+Custom protocol transmissions require type=<custom_type>"],
    )

    time_ok, _, cyccnt = runner.extract_time()
    request_id: Optional[int] = None

    if time_ok and cyccnt is not None:
        tx_cyccnt = (cyccnt + args.txat_offset_cycles) & 0xFFFFFFFF
        txat_cmd = (
            f":txat route=feedback protocol=custom payload=QQ== "
            f"tx_cyccnt={tx_cyccnt} type=bits"
        )
        ok, result = runner.run_command_test(
            "Schedule txat through feedback route",
            txat_cmd,
            [r"\[STATUS\]\s+OK :txat request_id=\d+ protocol=custom tx_cyccnt=\d+"],
            timeout_s=max(args.command_timeout_s, 2.0),
        )

        if ok:
            line = runner._first_match(result.lines, TXAT_OK_RE.pattern)
            if line is not None:
                match = TXAT_OK_RE.search(line)
                if match is not None:
                    request_id = int(match.group(1))
                    runner.run_condition_test(
                        "Parse txat request id",
                        True,
                        detail=f"request_id={request_id}",
                    )
                else:
                    runner.run_condition_test("Parse txat request id", False, "Regex match failed")
            else:
                runner.run_condition_test("Parse txat request id", False, "No txat status line found")

    if request_id is not None:
        runner.run_command_test(
            "Cancel scheduled feedback txat",
            f":cancel_tx {request_id}",
            [rf"\[STATUS\]\s+OK :cancel_tx request_id={request_id}"],
        )

    runner.run_command_test("Return to local mode", ":mode hostmac off", [r"\[STATUS\]\s+OK :mode hostmac off"])


def cleanup_session(session: SerialSession, timeout_s: float, settle_s: float) -> None:
    # Cleanup is best effort and intentionally ignores command outcomes.
    for cmd in (
        ":telemetrysub off",
        ":rxsub off",
        ":sense off",
        ":prompt off",
        ":tag on",
        ":mode hostmac off",
    ):
        try:
            session.send_command(cmd, timeout_s=timeout_s, settle_s=settle_s)
        except Exception:
            return


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Run OpenAquatix COMM command tests over serial while avoiding transducer-route invocations."
        )
    )
    parser.add_argument("port", help="Serial port (examples: /dev/ttyACM0, /dev/ttyUSB0, COM6)")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baudrate (default: 115200)")
    parser.add_argument(
        "--read-timeout-s",
        type=float,
        default=0.02,
        help="Per-read serial timeout in seconds (default: 0.02)",
    )
    parser.add_argument(
        "--command-timeout-s",
        type=float,
        default=1.5,
        help="Total wait time per command in seconds (default: 1.5)",
    )
    parser.add_argument(
        "--settle-s",
        type=float,
        default=0.20,
        help="Idle settle period after last RX byte in seconds (default: 0.20)",
    )
    parser.add_argument(
        "--skip-feedback-traffic",
        action="store_true",
        help="Skip :tx/:range/:txat feedback-route tests",
    )
    parser.add_argument(
        "--run-importcfg-negative",
        action="store_true",
        help="Include one negative :importcfg test with malformed input",
    )
    parser.add_argument(
        "--txat-offset-cycles",
        type=parse_int,
        default=100_000_000,
        help="CYCCNT offset added to :time cyccnt for txat test (default: 100000000)",
    )
    parser.add_argument("--verbose", action="store_true", help="Print command responses for each test")
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()

    print("Starting COMM command test harness")
    print(f"Port: {args.port}")
    print(f"Baud: {args.baud}")
    print(f"Skip feedback traffic tests: {args.skip_feedback_traffic}")

    try:
        session = SerialSession(args.port, args.baud, args.read_timeout_s)
    except Exception as exc:
        print(f"Failed to open serial port: {exc}", file=sys.stderr)
        return 2

    runner = SuiteRunner(
        session=session,
        timeout_s=args.command_timeout_s,
        settle_s=args.settle_s,
        verbose=args.verbose,
    )

    try:
        run_suite(runner, args)
    finally:
        cleanup_session(session, timeout_s=min(args.command_timeout_s, 1.0), settle_s=args.settle_s)
        session.close()

    print()
    print(f"Summary: {runner.pass_count} passed, {runner.fail_count} failed")
    if runner.fail_count > 0:
        print("Failed tests:")
        for record in runner.records:
            if not record.passed:
                print(f"- {record.name}")
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
