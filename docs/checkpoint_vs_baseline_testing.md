# Checkpoint vs. Baseline IPC Verification Guide

This note explains how to confirm whether ChampSim produces identical IPC when
you reuse cache checkpoints versus running the same trace segment in a
single, uninterrupted simulation. It covers both a manual workflow (useful
for one-off validation) and a scripted workflow for repeatable comparisons.

## Prerequisites
- ChampSim repository configured and built at least once (`./config.sh …`, `make`).
- Trace file accessible locally (e.g. `600.perlbench_s-210B.champsimtrace.xz`).
- Enough disk space to store warmup checkpoints and per-run statistics.

## Manual Workflow
1. **Warmup-only run to create the checkpoint**
   ```bash
   ./bin/champsim \
     --warmup-instructions 2000000 \
     --simulation-instructions 0 \
     --subtrace-count 3 \
     --cache-checkpoint base_cache.log \
     --json base_warmup.json \
     /path/to/trace.champsimtrace.xz
   ```
   - Produces `base_cache.log` containing warmed cache/TLB state.
   - Output JSON captures pre-measurement stats (handy for debugging).

2. **Window run from the checkpoint**
   ```bash
   cp base_cache.log window_cache.log
   ./bin/champsim \
     --warmup-instructions 100 \
     --simulation-instructions 6000000 \
     --subtrace-count 3 \
     --cache-checkpoint window_cache.log \
     --json checkpoint_window.json \
     /path/to/trace.champsimtrace.xz
   ```
   - `--warmup-instructions 100` is the resume warmup (tweak as needed).
   - Record IPC from `checkpoint_window.json` → field
     `sim.cores[0].ipc`.

3. **Single-pass baseline run**
   ```bash
   ./bin/champsim \
     --warmup-instructions 2000100 \
     --simulation-instructions 6000000 \
     --subtrace-count 3 \
     --json baseline_window.json \
     /path/to/trace.champsimtrace.xz
   ```
   - Warmup instructions cover full warmup (`2 000 000`) plus resume
     warmup (`100`) to ensure the same trace slice is measured.
   - Compare IPC in `baseline_window.json` with the checkpoint IPC.

4. **Analysis**
   - Expect the two IPC values to be very close. Any delta indicates the
     checkpointing flow affects cache/branch state; inspect MPKI and
     branch miss metrics in both JSONs to diagnose.

## Scripted Workflow
Use the helper script `rl_controller/compare_checkpoint.py` for batch
comparisons or multiple policy heads.

```bash
python -m rl_controller.compare_checkpoint \
  --trace /path/to/trace.champsimtrace.xz \
  --warmup 2000000 \
  --window 6000000 \
  --resume-warmup 100 \
  --action llc_replacement=lru,l2c_prefetcher=no \
  --action llc_replacement=srrip,l2c_prefetcher=next_line \
  --include-base \
  --output rl_runs/checkpoint_vs_baseline
```

- The script automatically:
  - Builds/uses policy-specific binaries via the RL harness.
  - Generates or reuses warmed checkpoints.
  - Runs each action twice:
    1. From the checkpoint (`resume_warmup` instructions then the window).
    2. As a standalone run (`warmup + resume_warmup` before the window).
  - Stores IPC/MPKI deltas in `comparison_summary.json` plus individual
    JSON stats files under the output directory.
- Override the standalone warmup with `--resume-solo` if you want a
  different baseline warmup length.

## Reading Results
- **Manual runs**: inspect `*_window.json` files; IPC lives at
  `sim.cores[0].ipc`.
- **Scripted runs**: check console output and the structured result in
  `comparison_summary.json`. Each entry includes:
  - Action name/value mapping.
  - Checkpoint IPC/metrics.
  - Standalone IPC/metrics.
  - Delta = `standalone - checkpoint`.

## Troubleshooting Tips
- **IPC mismatch larger than expected**: ensure the standalone warmup
  equals `warmup + resume_warmup`; mismatched instruction counts shift
  the trace window.
- **Deadlocks after restoring checkpoint**: increase `--resume-warmup`
  to flush queues before measurement.
- **Binary mismatch**: delete stale `bin/champsim_rl_*` binaries if you
  recently changed ChampSim modules; the builder will regenerate them.

