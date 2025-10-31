from __future__ import annotations

import argparse
import json
from dataclasses import asdict
from pathlib import Path
from typing import Dict, Iterable, List, Tuple
import subprocess

from .action_space import Action, ActionSpace, load_action_space
from .builder import ChampSimBuildManager
from .runner import ChampSimRunner
from .state import WindowMetrics, parse_stats_json


def _parse_action_spec(text: str) -> Dict[str, str]:
  mapping: Dict[str, str] = {}
  for chunk in text.split(","):
    chunk = chunk.strip()
    if not chunk:
      continue
    if "=" not in chunk:
      raise ValueError(f"Invalid action spec '{chunk}'. Expected format head=value.")
    head, value = chunk.split("=", 1)
    mapping[head.strip()] = value.strip()
  if not mapping:
    raise ValueError(f"Action spec '{text}' did not contain any assignments.")
  return mapping


def _dedupe_actions(actions: Iterable[Action]) -> List[Action]:
  seen: set[Tuple[Tuple[str, str], ...]] = set()
  unique: List[Action] = []
  for action in actions:
    key = tuple(sorted(action.values.items()))
    if key not in seen:
      seen.add(key)
      unique.append(action)
  return unique


def _metrics_to_dict(metrics: WindowMetrics) -> Dict[str, float]:
  return {key: float(value) for key, value in asdict(metrics).items()}


def _run_direct_window(
    repo_root: Path,
    binary_path: Path,
    trace_path: Path,
    warmup_instructions: int,
    window_instructions: int,
    stats_path: Path,
) -> WindowMetrics:
  stats_path.parent.mkdir(parents=True, exist_ok=True)
  cmd = (
      f"{binary_path} "
      f"--warmup-instructions {warmup_instructions} "
      f"--simulation-instructions {window_instructions} "
      f"--subtrace-count 1 "
      f"--json {stats_path} "
      f"{trace_path}"
  )
  subprocess.run(["bash", "-lc", cmd], cwd=repo_root, check=True)
  return parse_stats_json(stats_path)


def _format_action(action: Action) -> str:
  assignments = ", ".join(f"{name}={value}" for name, value in sorted(action.values.items()))
  return f"{{{assignments}}}"


def main() -> None:
  parser = argparse.ArgumentParser(description="Compare checkpointed ChampSim windows with standalone runs.")
  parser.add_argument("--trace", type=Path, required=True, help="Path to the ChampSim trace to simulate.")
  parser.add_argument("--warmup", type=int, required=True, help="Warmup instruction count for checkpoint creation.")
  parser.add_argument("--window", type=int, required=True, help="Measurement window instruction count.")
  parser.add_argument(
      "--resume-warmup",
      type=int,
      default=100,
      help="Instructions to run after restoring the checkpoint before measurement.",
  )
  parser.add_argument(
      "--action-space",
      type=Path,
      default=Path(__file__).with_name("action_space.json"),
      help="Path to JSON definition of the action space.",
  )
  parser.add_argument(
      "--output",
      type=Path,
      default=Path("rl_runs/compare_checkpoint"),
      help="Directory for checkpoints, stats, and summary output.",
  )
  parser.add_argument(
      "--shared-base",
      action="store_true",
      help="Reuse a single base checkpoint for all actions instead of per-action warmup.",
  )
  parser.add_argument(
      "--include-base",
      action="store_true",
      help="Include the action-space base policy in the comparison.",
  )
  parser.add_argument(
      "--action",
      dest="actions",
      action="append",
      default=[],
      help="Action specification 'head=value,head=value'. Repeat for multiple actions.",
  )
  parser.add_argument(
      "--resume-solo",
      type=int,
      default=None,
      help="Optional resume warmup for standalone runs. Defaults to full warmup (same as --warmup).",
  )
  args = parser.parse_args()

  repo_root = Path(__file__).resolve().parents[1]
  trace_path = args.trace.resolve()
  output_dir = args.output.resolve()
  checkpoint_dir = output_dir / "checkpoint"
  direct_dir = output_dir / "standalone"

  action_space, base_action, template_config = load_action_space(args.action_space.resolve())

  selected_actions: List[Action] = []
  if args.include_base:
    selected_actions.append(base_action)
  for spec in args.actions:
    mapping = _parse_action_spec(spec)
    selected_actions.append(action_space.from_dict(mapping))
  if not selected_actions:
    selected_actions.append(base_action)

  actions = _dedupe_actions(selected_actions)

  build_manager = ChampSimBuildManager(repo_root=repo_root, template_config=template_config)
  runner = ChampSimRunner(
      repo_root=repo_root,
      build_manager=build_manager,
      trace_path=trace_path,
      warmup_instructions=args.warmup,
      window_instructions=args.window,
      output_dir=checkpoint_dir,
      shared_base=args.shared_base,
      resume_warmup=args.resume_warmup,
  )

  base_checkpoint = runner.initialise_checkpoint(base_action, action_space)
  summary = {
      "trace": str(trace_path),
      "warmup_instructions": args.warmup,
      "window_instructions": args.window,
      "resume_warmup_checkpoint": args.resume_warmup,
      "resume_warmup_standalone": args.resume_solo if args.resume_solo is not None else args.warmup + args.resume_warmup,
      "shared_base": args.shared_base,
      "actions": [],
  }

  print(f"Loaded {len(actions)} action(s) to compare.")
  for idx, action in enumerate(actions):
    print(f"\n[{idx}] Comparing action {_format_action(action)}")

    checkpoint_result = runner.run_window(action, action_space, base_checkpoint, step=idx)
    checkpoint_metrics = _metrics_to_dict(checkpoint_result.metrics)

    direct_stats_path = direct_dir / f"iter_{idx:04d}_standalone.json"
    standalone_warmup = args.resume_solo if args.resume_solo is not None else args.warmup + args.resume_warmup
    build = build_manager.ensure_binary(action.as_config_updates(action_space.heads))
    direct_metrics = _run_direct_window(
        repo_root=repo_root,
        binary_path=build.binary_path,
        trace_path=trace_path,
        warmup_instructions=standalone_warmup,
        window_instructions=args.window,
        stats_path=direct_stats_path,
    )

    direct_metrics_dict = _metrics_to_dict(direct_metrics)
    delta = {key: direct_metrics_dict[key] - checkpoint_metrics[key] for key in checkpoint_metrics}

    print(
        f"  checkpoint IPC={checkpoint_metrics['ipc']:.6f} | "
        f"standalone IPC={direct_metrics_dict['ipc']:.6f} | "
        f"delta={delta['ipc']:+.6f}"
    )

    summary["actions"].append(
        {
            "values": dict(action.values),
            "checkpoint": checkpoint_metrics,
            "standalone": direct_metrics_dict,
            "delta": delta,
            "checkpoint_stats": str(checkpoint_result.stats_path),
            "standalone_stats": str(direct_stats_path),
            "checkpoint_cache": str(checkpoint_result.cache_path),
            "binary": str(build.binary_path),
        }
    )

  output_dir.mkdir(parents=True, exist_ok=True)
  summary_path = output_dir / "comparison_summary.json"
  with summary_path.open("w", encoding="utf-8") as handle:
    json.dump(summary, handle, indent=2)
  print(f"\nSaved comparison summary to {summary_path}")


if __name__ == "__main__":
  main()
