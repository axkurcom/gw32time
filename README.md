# GW32TIME

GW32TIME is a small Windows utility for inspecting, validating, and managing the
Windows Time service (`W32Time`). It provides both a graphical interface and a
CLI for common operator tasks: checking service health, reviewing configured NTP
servers, testing NTP quality, changing manual peers, applying safe presets,
backing up/restoring configuration, adjusting polling, requesting resync, and
setting the local date/time.

The project targets old and constrained Windows systems as well as modern
systems. The default build produces a 32-bit Windows executable with an XP-era
subsystem version and avoids SIMD instructions.

## Key Features

- GUI for Windows Time status and configuration.
- CLI for scripted and interactive operations.
- Health diagnostics for `W32Time` service state and basic registry settings.
- NTP server list parsing and editing with Windows peer flags.
- Built-in NTP checker with reachability, delay, offset, jitter, score, stratum,
  validation reason, IP, and PTR surface.
- Guardrails for domain-joined machines, aggressive polling, non-elevated
  writes, and invalid server names.
- Backup and restore of relevant `W32Time` configuration.
- UAC-assisted GUI operations for privileged changes.
- Release builds from semantic version tags such as `1.2.3`.

## Supported Platform

GW32TIME is a native Win32 application. The Makefile currently builds with the
`i686-w64-mingw32` toolchain.

Build characteristics:

- Target: `gw32time.exe`
- Architecture: i386 / 32-bit Windows
- Unicode entry point and UI strings
- Windows subsystem version: `5.1`
- Compiler flags disable SSE, SSE2, MMX, and related SIMD code generation
- `make verify` checks the subsystem version, binary size, and SIMD instruction
  surface

## GUI Overview

Running `gw32time.exe` without arguments opens the GUI.

The main window shows:

- Health status
- Windows Time service state
- Service start mode
- Synchronization type
- Poll interval controls
- Configured server table
- Live current local time
- UAC status indicator

Main actions:

- `Service`: start, stop, restart, and change service start mode.
- `Backup & Restore`: save or restore Windows Time configuration.
- `Sync Now`: request a `w32tm /resync`.
- `Add`, `Update`, `Delete`: edit NTP peer rows.
- `Apply Servers`: write the displayed peer list to W32Time.
- `Date & Time`: set local date/time through a privileged path.
- `Check Servers`: run NTP checks and populate validation metrics.
- `Realtime Check`: periodically re-check configured servers.

## Date & Time UI

The `Date & Time` action opens a focused editor for local date and time.

Behavior and guardrails:

- UAC is requested before entering the date/time editor when the current process
  cannot adjust system time.
- A successful UAC prompt is not treated as enough by itself; the elevated token
  must actually have `SeSystemtimePrivilege`.
- If the selected elevated account lacks the right to change system time, the
  helper is closed so the next click prompts for UAC again.
- If the current process already has the needed privilege, it sets time directly.
- The editor suppresses live refresh for fields the user has started editing.
- On first input into a field, the auto-filled value is cleared so only the
  user-entered digits remain.
- Input is length-limited: year is 4 digits; month, day, hour, minute, and
  second are 2 digits each.

## CLI Usage

```text
gw32time --help
gw32time --version
gw32time status [--raw] [--verbose]
gw32time gui
gw32time runtime
gw32time health
gw32time diag [--raw]
gw32time service status|start|restart
gw32time servers list
gw32time servers test <host>
gw32time checker <host...> [--samples N] [--timeout MS] [--interval MS] [--port N] [--parallel] [--explain] [--json]
gw32time servers set <host...> [--dry-run] [--yes] [--no-sync] [--force-domain]
gw32time poll get
gw32time poll set <seconds> [--dry-run] [--yes] [--force]
gw32time preset list
gw32time preset desktop|lab-fast|windows-default|domain [--dry-run] [--yes]
gw32time sane [--dry-run] [--yes]
gw32time backup <file>
gw32time restore <file> [--dry-run] [--yes]
gw32time menu
gw32time sync [--yes]
```

### Status and Diagnostics

Use:

```text
gw32time status
gw32time status --verbose
gw32time status --raw
gw32time health
gw32time diag
gw32time diag --raw
```

These commands read service state, start type, registry-backed W32Time settings,
and selected `w32tm` outputs. `health` condenses the result to one state:

- `OK`: service and basic configuration look usable.
- `WARNING`: configuration is suspicious but not necessarily unusable.
- `BROKEN`: service/configuration cannot work as-is.
- `UNKNOWN`: required state could not be queried.

Health checks include:

- Whether the Windows Time service state is readable.
- Whether the service start type is readable.
- Whether the service is disabled.
- Whether the service is running.
- Whether W32Time registry configuration is readable.
- Whether `Type=NoSync` is configured.
- Whether the NTP client provider is disabled.
- Whether NTP/manual modes have a non-empty server list.
- Whether `w32tm /query /status` completes successfully.

### Service Operations

```text
gw32time service status
gw32time service start
gw32time service restart
```

Changing service state requires an elevated administrator token. The GUI also
offers service start/stop/restart and start-mode actions.

### Server List Operations

```text
gw32time servers list
gw32time servers set time.cloudflare.com pool.ntp.org --dry-run
gw32time servers set time.cloudflare.com pool.ntp.org --yes
gw32time servers test time.cloudflare.com
```

`servers list` reads the configured `NtpServer` registry value and prints the
parsed peers and flags.

`servers set` builds a manual NTP peer list and applies it through both registry
write and `w32tm /config /manualpeerlist:... /syncfromflags:manual /update`.
After that it restarts `w32time` and, unless `--no-sync` is used, requests a
resync.

Default peer behavior:

- Each CLI peer is formatted with `0x8` client-mode flags.
- Host names may not be empty.
- Host names reject spaces, commas, quotes, backslashes, and slashes.
- The peer list is bounded by the project NTP peer limit.

### Polling Interval

```text
gw32time poll get
gw32time poll set 1024 --dry-run
gw32time poll set 1024 --yes
```

`poll get` prints known polling values, including `SpecialPollInterval` when
available.

`poll set` writes `SpecialPollInterval`, runs `w32tm /config /update`, and
restarts `w32time`.

Guardrails:

- Values below 64 seconds are rejected unless `--force` is used.
- Values below 256 seconds print an aggressive-polling warning.
- Values above one day print a long-polling warning.
- Applying changes requires elevation.
- Without `--yes`, the CLI asks for confirmation.

### Presets

```text
gw32time preset list
gw32time preset desktop --dry-run
gw32time preset lab-fast --yes
gw32time preset windows-default --yes
gw32time preset domain --yes
```

Available presets:

| Preset | Mode | Servers | Poll |
| --- | --- | --- | --- |
| `desktop` | `NTP` | `time.cloudflare.com,0x8 pool.ntp.org,0x8 time.google.com,0x8` | `1024` sec |
| `lab-fast` | `NTP` | `time.cloudflare.com,0x8 pool.ntp.org,0x8` | `256` sec |
| `windows-default` | `NTP` | `time.windows.com,0x8` | `604800` sec |
| `domain` | `NT5DS` | unchanged | unchanged |

Presets are applied by writing relevant W32Time registry values, running
`w32tm /config /update`, and restarting the service.

### Backup and Restore

```text
gw32time backup w32time-backup.ini
gw32time restore w32time-backup.ini --dry-run
gw32time restore w32time-backup.ini --yes
```

Backup writes the current W32Time configuration to a local file. Restore previews
the differences, asks for confirmation unless `--yes` is used, writes the saved
configuration back, runs `w32tm /config /update`, and restarts `w32time`.

## NTP Server Analysis

GW32TIME includes a custom NTP checker rather than relying only on `w32tm`
status. It actively sends NTP client requests and evaluates the responses.

### What Is Measured

For each server check, GW32TIME can report:

- `Reach`: successful samples over total samples.
- `Delay`: mean network delay.
- `Dmin`: minimum observed delay.
- `Offset`: median clock offset.
- `Mean`: mean offset.
- `Stddev`: standard deviation of filtered offsets.
- `MAD`: median absolute deviation of filtered offsets.
- `Jitter`: currently based on offset standard deviation.
- `Score`: normalized quality score from `0.00` to `1.00`.
- `Stratum`: NTP stratum from the response.
- `Valid`: whether a successful probe result is available.
- `Reason`: last validation or socket reason.
- `KoD/refid`: kiss-of-death reference ID when applicable.
- `IP`: resolved/probed IP surface in GUI rows.
- `PTR`: reverse DNS surface in GUI rows.

### Default Checker Configuration

The default checker configuration is:

- Samples: `5`
- Timeout: `800 ms`
- Interval between samples: `150 ms`
- Port: `123`
- Outlier threshold: `50 ms`

The simpler `gw32time servers test <host>` command uses the checker with a
shorter interval of `120 ms`.

### Protocol Validation

Each NTP response is validated before it contributes to statistics.

Rejected cases include:

- DNS failure.
- Socket failure.
- Timeout.
- Short packet.
- Response mode is not server mode.
- NTP version is not 3 or 4.
- Leap indicator is `3`, which means unsynchronized.
- Stratum is `0`, which is treated as kiss-of-death.
- Stratum is above `15`.
- Receive or transmit timestamp is zero.
- Originate timestamp is missing or does not match the request transmit
  timestamp.
- Non-primary stratum response has zero reference timestamp.
- Transmit timestamp is earlier than receive timestamp.
- Local receive timestamp is earlier than local send timestamp.
- Computed delay is less than `-1 ms`.

Small negative delay caused by precision or clock granularity is clamped to zero.

### Offset and Delay Formula

For a valid NTP sample, GW32TIME uses the standard four-timestamp model:

- `t1`: local send time
- `t2`: server receive time
- `t3`: server transmit time
- `t4`: local receive time

Offset:

```text
((t2 - t1) + (t3 - t4)) / 2
```

Delay:

```text
(t4 - t1) - (t3 - t2)
```

GW32TIME records both wall-clock NTP timestamps and a local monotonic RTT sample
for transport timing.

### Outlier Handling

Successful offsets are filtered with a median-based outlier filter. The default
threshold is `50 ms`. If filtering fails or removes all samples, the checker
falls back to all successful offsets instead of returning no statistics.

Reported statistics include median, mean, standard deviation, minimum delay,
mean delay, and MAD.

### Score

The score starts at `1.0`, is multiplied by reachability, and then receives
penalties:

- Minimum delay above `100 ms`: `-0.15`
- Jitter above `10 ms`: `-0.20`
- Absolute median offset above `100 ms`: `-0.25`
- Stratum `8` or higher: `-0.10`
- Any failed samples: `-0.05`

The final score is clamped to the `0.0` to `1.0` range.

### Explanations

With `--explain`, or through GUI detail surfaces, the checker can emit compact
notes such as:

- reachability percentage
- low/high jitter
- low/high delay
- median offset
- MAD
- KoD/refid detail

### IPv4 and IPv6

The checker resolves with `AF_UNSPEC`, so both IPv4 and IPv6 endpoints can be
used. It also accepts bracketed IPv6 literals, such as:

```text
gw32time checker [2001:4860:4806:8::]
```

For safety, a response is rejected if the sender address family does not match
the endpoint family being probed.

## Guardrails and Safety Model

GW32TIME is intentionally conservative around system time changes. It tries to
make risky operations explicit, previewable, and reversible.

### Elevation

Registry writes, service changes, resync operations that need service start, and
local time changes require a privileged token.

The GUI can start an elevated helper through UAC. For Date & Time, the helper is
not accepted only because UAC succeeded; it must prove it can enable
`SeSystemtimePrivilege`.

### Domain-Joined Machines

Manual NTP server changes are refused on domain-joined machines unless
`--force-domain` is supplied.

Reason: domain machines commonly receive time hierarchy and policy from the
domain. Forcing manual NTP can conflict with domain expectations or be reverted
by policy.

### Dry Runs

Most mutating CLI operations support `--dry-run`, including:

- `servers set`
- `poll set`
- `preset ...`
- `restore`
- `sane`

Dry-run output prints the current state, the desired state, and the exact action
plan.

### Confirmation

Mutating CLI operations ask for confirmation by default. Use `--yes` for
non-interactive automation.

### Polling Bounds

Very aggressive polling is blocked below `64` seconds unless `--force` is used.
Moderately aggressive values below `256` seconds warn the operator.

### Backup Before Restore

The restore path previews changes before writing them. Operators should keep a
backup made with:

```text
gw32time backup <file>
```

before applying large server/preset changes.

### Input Validation

Server names supplied to `servers set` are validated before formatting. Empty
hosts and hosts containing whitespace, commas, quotes, backslashes, or slashes
are rejected.

Date/time fields in the GUI are numeric and length-limited.

## Build

Install MinGW-w64 i686 tools and `make`, then run:

```text
make
```

The default output is:

```text
gw32time.exe
```

Clean generated artifacts:

```text
make clean
```

Verify the executable:

```text
make verify
```

`make verify` checks:

- PE subsystem major/minor version
- binary size
- absence of selected SIMD instruction mnemonics in disassembly

## Versioned Builds

The build accepts a `VERSION` variable:

```text
make VERSION=1.2.3
```

That version is compiled into:

- CLI `--version` output
- GUI main window title, for example `GW32TIME 1.2.3`

Changing `VERSION` creates a version stamp under `build/` so the executable is
relinked when the version changes.

## GitHub Actions

### Try Workflow

`.github/workflows/try.yml` runs on every push and pull request.

It has two independent jobs so GitHub Actions can run them in parallel:

- `Test build`: builds `build/gw32time-test.exe` using the existing Makefile
  with `TARGET=build/gw32time-test.exe` and `STRIP=:` overrides.
- `Smoke tests`: runs `bash scripts/smoke/run.sh`.

### Release Workflow

`.github/workflows/release.yml` runs on pushed tags matching `*.*.*` and then
validates that the tag is exactly semantic-version-like:

```text
1.2.3
```

The release workflow:

1. Checks out the repository.
2. Validates the tag.
3. Installs the MinGW toolchain.
4. Builds with `make VERSION="$VERSION"`.
5. Runs `make VERSION="$VERSION" verify`.
6. Packages `gw32time.exe`.
7. Writes a SHA-256 checksum.
8. Uploads a GitHub Actions artifact.
9. Publishes a GitHub Release with the executable and checksum.

Tests are intentionally not duplicated in `release.yml` because the `try.yml`
workflow already runs on the push.

## Smoke Tests

The smoke runner is:

```text
bash scripts/smoke/run.sh
```

Current smoke checks cover:

- Build and `make verify`.
- CLI checker command surface.
- GUI NTP table columns and checker fields.
- IPv6-ready NTP socket surface.

The smoke scripts are source-surface and build-surface checks. They are designed
to be stable in CI without needing live network access to a specific NTP server.

## Artifact Hygiene

Generated build outputs are ignored by `.gitignore`, including:

- `build/`
- `dist/`
- `release/`
- `*.exe`
- `*.dll`
- `*.o`
- `*.obj`
- `*.res`
- release archives and checksums
- linker/debug outputs
- local build logs

## Operational Notes

- Prefer `--dry-run` before changing servers, presets, polling, or restoring a
  backup.
- Use `gw32time health` before making changes to understand the current failure
  mode.
- Use `gw32time servers test <host>` or `gw32time checker <host...>` before
  applying a new NTP server list.
- On domain-joined machines, prefer the `domain` preset unless there is an
  explicit reason to override domain time hierarchy.
- Keep polling conservative on production systems.
- If resync fails, inspect `gw32time diag --raw` and the raw `w32tm` output.

## Troubleshooting

### Sync Requires Elevation

Run the CLI from an elevated console or use the GUI UAC flow.

### Server Changes Refused on Domain Machine

Use `--force-domain` only when you intentionally want manual NTP on a
domain-joined system.

### No Successful NTP Samples

Check:

- DNS resolution.
- UDP/123 reachability.
- Local firewall policy.
- Whether the server returns KoD.
- Whether the server response has valid mode/version/timestamps.
- Whether IPv6 routing is available when probing an IPv6 literal.

### High Jitter or Poor Score

Try multiple servers and compare:

- reachability
- minimum delay
- median offset
- MAD
- jitter
- stratum

Prefer a server with high reachability, low delay, low jitter, small absolute
median offset, and a reasonable stratum.

