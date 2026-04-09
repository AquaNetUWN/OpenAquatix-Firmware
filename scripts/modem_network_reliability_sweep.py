#!/usr/bin/env python3
"""
Parameter sweep wrapper for modem network reliability testing.

This script mutates a base START_ALL/END_ALL (or START_SOME/END_SOME) config
blob across combinations of parameter IDs and values, runs the multi-node
reliability test for each combination, and reports the best-performing setup.
"""

from __future__ import annotations

import argparse
import itertools
import json
from collections import OrderedDict
from pathlib import Path
from typing import Any, Optional

from modem_network_reliability import (
    DEFAULT_NODE_HOSTS,
    load_config_blob,
    parse_config_blob,
    parse_node_specs,
    render_config_blob,
    run_reliability_campaign,
)


def normalize_override_map(raw: dict[Any, Any]) -> OrderedDict[int, str]:
    result: OrderedDict[int, str] = OrderedDict()
    for key, value in raw.items():
        param_id = int(key)
        result[param_id] = str(value)
    return result


def load_sweep_combinations(spec_path: str) -> list[OrderedDict[int, str]]:
    raw = json.loads(Path(spec_path).read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise ValueError("Sweep spec must be a JSON object")

    fixed_overrides = normalize_override_map(raw.get("fixed_overrides", {}))

    combinations: list[OrderedDict[int, str]] = []
    if "combinations" in raw:
        explicit = raw["combinations"]
        if not isinstance(explicit, list) or not explicit:
            raise ValueError("combinations must be a non-empty JSON list")
        for combo in explicit:
            if not isinstance(combo, dict):
                raise ValueError("Each combinations entry must be a JSON object")
            merged = OrderedDict(fixed_overrides)
            merged.update(normalize_override_map(combo))
            combinations.append(merged)
        return combinations

    dimensions = raw.get("parameters")
    if not isinstance(dimensions, list) or not dimensions:
        raise ValueError(
            "Sweep spec must contain either 'combinations' or a non-empty 'parameters' list"
        )

    param_ids: list[int] = []
    param_values: list[list[str]] = []

    for item in dimensions:
        if not isinstance(item, dict):
            raise ValueError("Each parameters entry must be an object with id and values")

        if "id" not in item or "values" not in item:
            raise ValueError("Each parameters entry requires id and values")

        param_id = int(item["id"])
        values = item["values"]
        if not isinstance(values, list) or not values:
            raise ValueError(f"Parameter id {param_id} has an empty values list")

        param_ids.append(param_id)
        param_values.append([str(v) for v in values])

    for combo_values in itertools.product(*param_values):
        combo_map = OrderedDict(fixed_overrides)
        for param_id, value in zip(param_ids, combo_values):
            combo_map[param_id] = value
        combinations.append(combo_map)

    return combinations


def apply_overrides(
    base_params: OrderedDict[int, str],
    overrides: OrderedDict[int, str],
) -> OrderedDict[int, str]:
    merged = OrderedDict(base_params)
    for param_id, value in overrides.items():
        merged[param_id] = value
    return merged


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Sweep modem config parameter combinations and rank reliability"
    )

    parser.add_argument(
        "--nodes",
        default=",".join(DEFAULT_NODE_HOSTS),
        help="Comma-separated hostnames (default: node-1.local,node-2.local,node-3.local,node-4.local)",
    )
    parser.add_argument("--node-config-file", help="Optional JSON list of per-node SSH/serial settings")
    parser.add_argument("--device", default="/dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--ssh-user")
    parser.add_argument("--ssh-port", type=int, default=22)
    parser.add_argument("--ssh-option", action="append", default=[])

    parser.add_argument("--config-blob", help="Base configuration blob")
    parser.add_argument("--config-file", help="Path to base configuration blob file")

    parser.add_argument("--sweep-spec-file", required=True, help="JSON file describing parameter sweep")
    parser.add_argument(
        "--max-combinations",
        type=int,
        default=0,
        help="Optional hard cap on number of combinations (0 means no cap)",
    )
    parser.add_argument("--tx-route", choices=["transducer", "feedback"], default="transducer")
    parser.add_argument("--messages-per-node", type=int, default=10)
    parser.add_argument("--include-sender-receive", action="store_true")
    parser.add_argument("--payload-prefix", default="SWEEP")
    parser.add_argument("--read-timeout-s", type=float, default=0.05)
    parser.add_argument("--command-timeout-s", type=float, default=2.0)
    parser.add_argument("--settle-s", type=float, default=0.25)
    parser.add_argument("--receive-timeout-s", type=float, default=20.0)
    parser.add_argument("--inter-send-delay-s", type=float, default=0.25)

    parser.add_argument("--stop-on-perfect", action="store_true")
    parser.add_argument("--results-json", help="Optional JSON output path for sweep results")
    parser.add_argument(
        "--combo-summary-dir",
        help="Optional directory to write full per-combo run summary JSON files",
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
        base_blob = load_config_blob(args.config_blob, args.config_file)
        start_marker, end_marker, base_params = parse_config_blob(base_blob)
        combos = load_sweep_combinations(args.sweep_spec_file)
    except Exception as exc:
        print(f"Error: {exc}")
        return 2

    if args.max_combinations > 0:
        combos = combos[: args.max_combinations]

    if not combos:
        print("No combinations to run")
        return 2

    if args.combo_summary_dir:
        Path(args.combo_summary_dir).mkdir(parents=True, exist_ok=True)

    print(f"Sweep combinations: {len(combos)}")

    results: list[dict[str, Any]] = []
    for index, overrides in enumerate(combos, start=1):
        merged_params = apply_overrides(base_params, overrides)
        combo_blob = render_config_blob(start_marker, end_marker, merged_params)
        combo_label = f"combo_{index:04d}"

        print(f"[{index}/{len(combos)}] running {combo_label} overrides={dict(overrides)}")
        try:
            summary = run_reliability_campaign(
                node_specs=node_specs,
                config_blob=combo_blob,
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

            result_entry: dict[str, Any] = {
                "combo_label": combo_label,
                "overrides": dict(overrides),
                "delivery_reliability": summary["delivery_reliability"],
                "full_delivery_rate": summary["full_delivery_rate"],
                "successful_deliveries": summary["successful_deliveries"],
                "expected_deliveries": summary["expected_deliveries"],
                "full_delivery_transmissions": summary["full_delivery_transmissions"],
                "total_transmissions": summary["total_transmissions"],
                "summary_timestamp_utc": summary["timestamp_utc"],
            }
            results.append(result_entry)

            if args.combo_summary_dir:
                out_path = Path(args.combo_summary_dir) / f"{combo_label}.json"
                out_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")

            print(
                f"  reliability={summary['delivery_reliability']:.4f} "
                f"full_delivery_rate={summary['full_delivery_rate']:.4f}"
            )

            if args.stop_on_perfect and summary["delivery_reliability"] >= 1.0:
                print("Perfect reliability reached; stopping early")
                break

        except Exception as exc:
            results.append(
                {
                    "combo_label": combo_label,
                    "overrides": dict(overrides),
                    "error": str(exc),
                }
            )
            print(f"  failed: {exc}")

    ranked = sorted(
        (entry for entry in results if "error" not in entry),
        key=lambda item: (item["delivery_reliability"], item["full_delivery_rate"]),
        reverse=True,
    )

    print()
    print("=== Sweep Summary ===")
    print(f"Completed combinations: {len(results)}")
    print(f"Successful runs: {len(ranked)}")
    print(f"Failed runs: {sum(1 for entry in results if 'error' in entry)}")

    if ranked:
        best = ranked[0]
        print("Best combination:")
        print(f"  {best['combo_label']} overrides={best['overrides']}")
        print(f"  delivery_reliability={best['delivery_reliability']:.4f}")
        print(f"  full_delivery_rate={best['full_delivery_rate']:.4f}")
    else:
        best = None
        print("No successful runs were recorded")

    if args.results_json:
        output_payload = {
            "base_config_markers": {"start": start_marker, "end": end_marker},
            "node_names": [node.name for node in node_specs],
            "tx_route": args.tx_route,
            "messages_per_node": args.messages_per_node,
            "results": results,
            "ranked": ranked,
            "best": best,
        }
        Path(args.results_json).write_text(json.dumps(output_payload, indent=2), encoding="utf-8")
        print(f"Sweep JSON written to {args.results_json}")

    return 0 if best is not None else 1


if __name__ == "__main__":
    raise SystemExit(main())
