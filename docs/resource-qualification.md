# Resource qualification

`resource-smoke` is a bounded full-stack resource gate for the existing Linux
binaries. It does not build Mux and it does not change runtime configuration or
production behavior. The fixture uses loopback HTTP, headless Weston with the
Pixman renderer, real Kitty, real WPE WebKit, `muxd`, one profile engine, two
panes, and the global bar.

## Run it

Point the gate at an existing release build:

```sh
MUX_BIN_DIR="$PWD/build/runtime-smoke" ./resource-smoke
```

The normal path must run as a non-root Linux user. A disposable CI container is
the only root exception:

```sh
CI=true MUX_RESOURCE_ALLOW_ROOT_CI=1 \
  MUX_BIN_DIR=/work/build/resource-smoke \
  ./resource-smoke
```

The command has an outer 180 second timeout. The measured workload is 15
seconds of visible canvas animation followed by 20 seconds of an idle static
layer while the animated layer is hidden. Four-second warmup and settling
periods are excluded from CPU and memory calculations.

## Default budgets

`100% CPU` means one fully occupied logical core. CPU is the sum of user and
system ticks for every process in scope divided by wall time.

| Measurement | Default budget |
| --- | ---: |
| Idle aggregate CPU | 25% of one core |
| Active aggregate CPU | 220% on ARM64; 425% on x86_64 |
| Peak aggregate RSS | 1800 MiB |
| Peak aggregate PSS, when readable | 1280 MiB |
| Peak scoped process count | 48 |
| Peak new named `/dev/shm` bytes | 64 MiB |
| Post-cleanup `/dev/shm` leakage | 0 bytes and 0 entries |
| Visible active-frame acknowledgements | at least 10 |
| Hidden animated-layer acknowledgements | at most 1 after settling |
| Graceful closure of both panes | 10 seconds |
| TERM cleanup of the full process scope | 10 seconds |

The active acknowledgement floor prevents a false low-CPU pass when rendering
is stalled. The same animation remains loaded for the idle phase, but its Kitty
layer is hidden. Its acknowledgement delta is reported and enforced separately
to qualify hidden-layer suspension.

The active fixture deliberately drives an unthrottled full-screen canvas through
the software-rendered headless stack. Its ceiling is architecture-specific
because x86_64 GitHub runners use substantially more aggregate WPE and Kitty CPU
for that workload. Idle, memory, process, shared-memory, and cleanup limits are
architecture-independent.

## Measurement scope and method

The sampler starts from five fixture roots: Weston, the loopback HTTP server,
`muxd`, `mux-engine`, and Kitty. Every descendant is included, so the scope
covers WebKit subprocesses, both panes, the bar, and compositor/presentation
costs. The sampler itself and the D-Bus wrapper are excluded.

Once per second, the sampler reads:

- CPU ticks from `/proc/<pid>/stat`.
- RSS from `/proc/<pid>/status`.
- PSS from `/proc/<pid>/smaps_rollup` when all scoped processes are readable.
- Process identity as PID plus `/proc` start time, preventing PID-reuse errors.
- New named entries and apparent bytes relative to the initial `/dev/shm`
  snapshot.

PSS is enforced whenever at least one complete aggregate sample is available.
If kernel permissions hide any `smaps_rollup` for every sample, the report says
`unavailable` and the always-available RSS ceiling remains enforced. Short-lived
unlinked shared-memory mappings are represented in RSS/PSS; named-object leakage
is independently checked by returning `/dev/shm` to its exact baseline.

Cleanup first closes both views through Kitty and requires `muxctl status` to
report `views=0`. It then records every scoped PID/start-time identity, sends
TERM to the fixture roots, waits for the process budget, audits sockets and
`/dev/shm`, and treats a required SIGKILL fallback as a failure.

## Determinism and diagnostics

Every wait, sampling interval, and cleanup stage is bounded. Budget comparisons
use phase aggregates rather than a single instantaneous CPU sample. A failure
prints the measured value, limit, top CPU/RSS/PSS consumers, Mux status, and the
tail of each fixture log.

Machine-readable artifacts include:

- `active.json` and `idle.json` with phase totals and per-process maxima.
- `tracked-processes.tsv` with stable process identities.
- `shm-baseline.json` and `shm-final.json`.
- `summary.txt` and `budgets.txt`.

Set `MUX_RESOURCE_ARTIFACT_DIR` to copy artifacts to a stable directory. Set
`MUX_RESOURCE_KEEP_ARTIFACTS=1` to retain the temporary directory printed by the
script. CI copies artifacts to `resource-smoke-artifacts` and uploads them even
when qualification fails.

All defaults have matching environment overrides named in `resource-smoke`,
for example `MUX_RESOURCE_IDLE_SECONDS` and
`MUX_RESOURCE_IDLE_CPU_PCT`. Overrides are intended for local diagnosis. A CI
budget change should be reviewed as a qualification-policy change, not used to
make a regression green.

## ARM64 Docker

The local ARM64 dependency image can run the gate against a read-only mounted
binary directory. It needs the same sandbox permissions and shared-memory size
as runtime smoke:

```sh
docker run --rm --privileged --userns=host \
  --security-opt seccomp=unconfined --shm-size=2g \
  -e CI=true -e MUX_RESOURCE_ALLOW_ROOT_CI=1 \
  -e MUX_BIN_DIR=/mux-bin \
  -v "$PWD:/work:ro" -v /path/to/prebuilt/mux:/mux-bin:ro \
  -w /work mux-alarm-smoke-cached:latest ./resource-smoke
```

Use binaries built for the container architecture. The harness deliberately
does not invoke Meson or compile inside the measurement container.
