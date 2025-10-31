# RL Controller for ChampSim

This folder provides a minimal harness that

1. builds policy-specific ChampSim binaries on demand,
2. creates a reusable cache checkpoint after warmup,
3. runs fixed-size simulation windows starting from that checkpoint, and
4. records per-window statistics together with the chosen action.

The default action space includes two heads:

| head name        | JSON path          | choices (editable)          |
|------------------|--------------------|-----------------------------|
| `l2c_prefetcher` | `["L2C","prefetcher"]` | `no`, `next_line`, `ip_stride` |
| `llc_replacement`| `["LLC","replacement"]`| `lru`, `srrip`, `random`        |

You can edit `action_space.json` to add or remove discrete options and to
change the warmup policy used for the initial checkpoint.

## Usage

```bash
python -m rl_controller.main \
  --trace /path/to/trace.champsimtrace.xz \
  --warmup 10000000 \
  --window 50000000 \
  --steps 5 \
  --output rl_runs/perlbench \
  --resume-warmup 100
```

By default each action gets its own warmup checkpoint.  Add `--shared-base`
to reuse a single checkpoint for all actions (handy for strict A/B testing,
but only safe if every policy can resume from the same cache image).

The `--resume-warmup` knob controls how many instructions are run before each
measurement window after the checkpoint is restored.  Keeping it small (e.g.
`1` or `100`) avoids deadlocks while leaving the cache contents largely intact.

The script prints one line per step with the chosen action and IPC, and
stores detailed statistics in `rl_runs/.../iter_XXXX_stats.json`.  A
summary of the episode lives in `episode_summary.json`.

## Checkpoint vs. Standalone comparison

To quantify the effect of reusing cache checkpoints versus running each
policy from scratch, use the helper script below.  It warmups the base
policy once, replays each action window from that checkpoint, and then
executes a full standalone run for the same action to highlight any IPC
differences.

```bash
python -m rl_controller.compare_checkpoint \
  --trace /path/to/trace.champsimtrace.xz \
  --warmup 10000000 \
  --window 50000000 \
  --action l2c_prefetcher=next_line,llc_replacement=srrip \
  --action l2c_prefetcher=ip_stride,llc_replacement=lru \
  --include-base \
  --output rl_runs/compare_perlbench
```

- `--include-base` adds the base action from `action_space.json` to the
  comparison set.
- By default, the standalone runs execute `warmup + resume_warmup`
  instructions before the measurement window; override with
  `--resume-solo` if you prefer a different warmup length.
- Results and per-run statistics are saved under the chosen output
  directory together with a machine-readable `comparison_summary.json`.

### Notes

- The harness reuses a warm checkpoint created once with the base action.
  Each window restores that state before running the measurement phase,
  so you can compare policies under identical cache contents.
- New ChampSim binaries are generated lazily and cached (for example
  `bin/champsim_rl_llc-replacement-srrip_l2c-prefetcher-next_line`).  If
  you change module code, remove the corresponding binary to force a
  rebuild.
- Replace the default `RandomAgent` with your own agent to perform online
  learning.  Each `RunResult` includes a feature vector you can feed into
  an RL algorithm.
