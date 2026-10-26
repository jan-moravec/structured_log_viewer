# Manual test plan: slp and StructuredLogViewer

This plan exercises a packaged build of `slp` and `StructuredLogViewer` with
data from `log_generator`. The `slp` part is scripted; the GUI part is a
checklist. All commands are PowerShell 7.4+ and run from the repository root.

| File                   | Purpose                                                            |
| ---------------------- | ------------------------------------------------------------------ |
| `New-TestFixtures.ps1` | Generates the seeded fixtures into `build\manual-test`.            |
| `Test-Slp.ps1`         | Runs the `slp` scenarios below and prints PASS / FAIL / INFO rows. |

## Setup

1. Build the generator (the packaged build does not include it):

   ```powershell
   .\scripts\Enter-DevShell.ps1 -Arch amd64
   cmake --build --preset release --target log_generator
   ```

1. Put the packaged build in `build\StructuredLogViewer`. A build downloaded
   from GitHub carries the Mark of the Web, and Windows may block it from
   starting. Unblock it once:

   ```powershell
   Get-ChildItem -Recurse .\build\StructuredLogViewer | Unblock-File
   ```

1. Generate the fixtures and define shortcuts for the commands below:

   ```powershell
   .\test\manual\New-TestFixtures.ps1
   $gen = '.\build\release\bin\Release\log_generator.exe'
   $slp = '.\build\StructuredLogViewer\slp.exe'
   $slv = '.\build\StructuredLogViewer\StructuredLogViewer.exe'
   $fx  = '.\build\manual-test'
   ```

## Fixtures

All fixtures use a simulated clock starting at 2026-03-01 00:00 and a fixed
seed, so the counts below are stable. Timestamps carry no zone and are read as
UTC.

| File                      | Content                                                      | Useful counts                                                     |
| ------------------------- | ------------------------------------------------------------ | ----------------------------------------------------------------- |
| `app.jsonl`               | 1,000 JSON lines                                             | error+fatal 44; `component:database` 202; both 9                  |
| `fmt_<format>.*`          | 2,000 lines each: logfmt, csv, syslog, apache-\*, java       | error+fatal 89 (logfmt, csv, java)                                |
| `day.jsonl`               | one day at 2 lines/s average, daily profile, bursts          | 222,019 rows; warn+ 63,726; error+ 36,143; 13:00-14:00 UTC 15,664 |
| `rot\app.jsonl` (+ .1-.3) | 5,000 lines rotated every 1,000, 3 backups kept              | 4,000 rows with history (`line_number` 1000-4999); 1,000 without  |
| `big.jsonl`               | 1,000,000 JSON lines, about 200 MB (`-SkipLarge` to omit it) | error+fatal 47,448; `message ~ /^lorem ipsum/` 2,534              |

Every record has a `line_number` that counts from 0 and continues across
rotations. A gap in `line_number` means a lost line.

## slp

Run the whole set:

```powershell
.\test\manual\Test-Slp.ps1            # add -SkipTail to skip the 1-minute streaming part
```

The script derives expected values from the data, so it keeps working when
the generator changes. It covers:

| Area     | Scenario                                                                                         | Example command                                                            |
| -------- | ------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------- |
| validate | Every format is detected, every line becomes a row, no parse errors, one time column.            | `& $slp validate $fx\fmt_syslog.log`                                       |
| filter   | `--min-level`, `level IN [...]`, `--invert`, numeric ranges, and `stats` counts agree.           | `& $slp query --count --filter 'level IN [error, fatal]' $fx\app.jsonl`    |
| query    | `--sort`, `--limit`, `--last`, `--columns`, csv and markdown output.                             | `& $slp query --sort line_number:desc --limit 3 -t markdown $fx\app.jsonl` |
| time     | `--since/--until` match a `timestamp IN [..]` filter; `Z` and `+00:00` agree; daily profile.     | `& $slp stats --by time --bucket 1h --tz UTC $fx\day.jsonl`                |
| rotation | Rotated siblings load oldest first, `--no-rotation-history` loads one file, no gaps.             | `& $slp query --count $fx\rot\app.jsonl`                                   |
| stdin    | JSON and logfmt piped from the generator are counted and detected.                               | `& $gen -q -n 5000 -o - \| & $slp query --count -`                         |
| exit     | Exit codes 1, 2, 3, and 5 for empty result, bad filter or column, missing source, existing file. | `& $slp query $fx\missing.jsonl; $LASTEXITCODE`                            |
| bundle   | A bundle keeps its filter; `--filtered-rows` keeps only matches; both read back with `slp`.      | `& $slp bundle --min-level warn -o $fx\work\incident $fx\day.jsonl`        |
| tail     | Growing file, TCP, UDP, format sniffing on TCP, and a file rotating under each strategy.         | see below                                                                  |

Streaming by hand needs two terminals:

```powershell
# Terminal 1: listen.
& $slp tail tcp://127.0.0.1:5141
# Terminal 2: send 1,000 lines at 200 lines/s.
& $gen -n 1000 --rate 200 -o tcp://127.0.0.1:5141

# Follow a file while it rotates; line_number must not jump.
& $slp tail --no-rotation-history $fx\work\live.jsonl
& $gen --rate 400 -n 2000 --roll-lines 300 --roll-strategy rename -o $fx\work\live.jsonl
```

Windows reserves some port ranges (`netsh int ipv4 show excludedportrange protocol=udp`).
If a listener reports "access permissions", pick another port with
`-TcpPort` / `-UdpPort`.

## StructuredLogViewer

Use `--new-instance` so each launch is independent of a running window.
Expected counts appear in the status bar after filtering.

### G1. Formats

```powershell
foreach ($f in Get-ChildItem $fx\fmt_*) { & $slv --new-instance $f.FullName }
```

- [ ] Each window shows 2,000 rows and a timestamp column rendered as a date.
- [x] logfmt, csv, and java show a level column with level colours; error+fatal is 89.
- [x] syslog splits hostname, program, and pid; apache files show request fields.
- [x] apache-error shows the level as a level (see finding 4).

### G2. Filters

Open `app.jsonl` and use **Filters → Advanced Filter…**:

- [x] `level IN [error, fatal]` → 44 rows.
- [x] `component:database AND level IN [error, fatal]` → 9 rows.
- [x] `NOT component:database` → 798 rows.
- [x] `line_number IN [100..199]` → 100 rows.
- [x] A broken query such as `level IN [` shows a parse error and disables OK.

### G3. Time and histogram

Open `day.jsonl`, then **View → Histogram** (`Ctrl+H`):

- [x] The histogram shows a day/night curve: low around 02:00 UTC, high around 14:00 UTC (shifted by your offset from UTC).
- [x] Short spikes dominated by warning and error colours are visible (bursts).
- [x] Dragging across 13:00-14:00 UTC filters to 15,664 rows.
- [x] **Edit → Go to Timestamp…** with `2026-03-01T13:00:00Z` lands on the first row at or after 13:00 UTC.

### G4. Rotation history

```powershell
& $slv --new-instance $fx\rot\app.jsonl
& $slv --new-instance --no-rotation-history $fx\rot\app.jsonl
```

- [x] With history: 4,000 rows, `line_number` from 1000 to 4999 in order.
- [x] Without history: 1,000 rows (4000-4999).

### G5. Large file

```powershell
& $slv --new-instance $fx\big.jsonl
```

- [x] Opens without freezing; note the time to the full row count (1,000,000).
- [x] Scrolling to the end and back is smooth; memory stays reasonable (Task Manager).
- [x] `level IN [error, fatal]` → 47,448; `message ~ /^lorem ipsum/` → 2,534.

### G6. Standard input

```powershell
& $gen -n 5000 --rate 200 -o - | & $slv --new-instance -
& $gen -f logfmt -n 2000 --rate 100 -o - | & $slv --new-instance -
```

- [x] Rows appear while the generator is running, not only at the end.
- [x] The final row count is 5,000 (2,000 for logfmt) and logfmt is detected.

### G7. Stream Mode on a rotating file

Create the file, open it with **File → Open Log Stream…** (`Ctrl+Shift+O`),
then start the writer:

```powershell
New-Item -Force $fx\work\live.jsonl | Out-Null
& $gen --rate 200 --forever --roll-size 256KiB --roll-strategy rename -o $fx\work\live.jsonl
```

- [x] Rows keep arriving across rotations; Pause, Follow newest, and Stop work.
- [x] After Stop, sort by `line_number`: no gaps with `rename`.
- [x] Repeat with `--roll-strategy copytruncate` and `truncate`; a few lines may be lost at each truncation (known limitation), but the stream must continue.

### G8. Network streams

Open **File → Open Network Stream…** (`Ctrl+Shift+L`) and start the listener before sending.

```powershell
& $gen -n 1000 --rate 200 -o tcp://127.0.0.1:5141          # TCP, Auto-detect
& $gen -n 1000 --rate 200 -o udp://127.0.0.1:5142          # UDP, Auto-detect
& $gen -f logfmt -n 1000 --rate 200 -o tcp://127.0.0.1:5141 # logfmt, Auto-detect
```

- [x] TCP and UDP JSON: 1,000 rows, no `line_number` gaps.
- [x] Two generators sending to the same TCP port at once both arrive; no torn lines.
- [x] logfmt with Auto-detect, sent more than a few seconds after the listener started, is parsed as logfmt (see finding 3).

TLS, with a self-signed certificate:

```powershell
& 'C:\Program Files\Git\usr\bin\openssl.exe' req -x509 -newkey rsa:2048 -nodes -days 1 `
    -keyout $fx\work\key.pem -out $fx\work\cert.pem -subj '/CN=localhost'
# Dialog: TCP, port 5143, Enable TLS, certificate cert.pem, key key.pem.
& $gen -n 100 --rate 20 -o tcp+tls://127.0.0.1:5143 --tls-skip-verify
```

- [x] 100 rows arrive. Without `--tls-skip-verify` the generator refuses the self-signed certificate.

### G9. Session bundles

```powershell
& $slp bundle --min-level warn --sort timestamp:desc -o $fx\work\incident $fx\day.jsonl
& $slv --new-instance $fx\work\incident.slvbundle
```

- [x] The bundle opens without the original log; the warn+ filter (63,726 rows) and descending sort are restored.
- [x] Clearing the filter shows all 222,019 rows.
- [x] **File → Export Session Bundle…** from the GUI, then `& $slp query --count <that file>` gives the same count.

### G10. Time zones

- [ ] With the display zone set to UTC, `app.jsonl` shows the first row at 2026-03-01 00:00:00.082, matching `& $slp query --tz UTC --limit 1 $fx\app.jsonl`.
- [x] With the local zone, GUI and `slp` (default `--tz local`) show the same times.

## Findings from the first run

Run on 2026-10-26 with the packaged `slp 0.16.0`. Findings 1-5 are fixed on
`feature/slp-cli`; `Test-Slp.ps1` checks each one, so a packaged build from
before the fixes still fails those checks.

1. **Fixed: `--since`/`--until` ignored a trailing `Z`.** With a local zone of
   UTC+1, `--since 2026-03-01T13:00:00Z --until 2026-03-01T14:00:00Z` returned
   the 12:00-13:00 UTC hour (17,093 rows instead of 15,664). A value ending in
   `Z` is now read as UTC (`c830afc`); the GUI's Go to Timestamp shares the fix.
1. **Fixed (docs): `--tz` was documented as the zone for naive log
   timestamps.** loglib, the GUI, and bundles read zone-less log timestamps as
   UTC. `doc/cli.md` and `--help` now say `--tz` sets the display zone and the
   zone for zone-less `--since`/`--until` values (`c830afc`).
1. **Fixed: network auto-detect gave up when the first sender was late.**
   Detection ran its 500 ms peek from listener start, so a sender arriving
   later was parsed as JSON and every line failed. The peek now starts at the
   first byte, for `slp tail` and the GUI (`10543c5`). Stdin still times out
   500 ms after start.
1. **Fixed: the `apache_error` template captured `module:level` as the
   level.** Apache 2.4 lines now give separate `module` and `level` columns,
   so levels resolve and `--min-level` works (`39c4a5f`). The generator's
   `apache-error` format now also writes some legacy `[level]` lines.
1. **Fixed: `validate` reported one line too many** for JSON, CSV, syslog, and
   the Apache access formats (`39c4a5f`).
1. **Open: `stats` prints `span: N row(s)` without a time range** for
   `--by level`; `--by time` prints the actual span.

### GUI results

Run on 2026-10-26 with `build\release\bin\Release\StructuredLogViewer.exe`
built from `feature/slp-cli` at `263ab5f`, on Windows 11 with a local zone of
UTC+1. Unticked boxes above failed or were not run.

| Step | Result                                                                                                                                                                       |
| ---- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| G1   | Fail. Every file has 2,000 rows; levels, syslog, and Apache fields are right; error+fatal is 89. apache-error timestamps stay text (GUI finding 1).                          |
| G2   | Pass: 44, 9, 798, 100; `level IN [` shows a parse error at position 11 and disables OK.                                                                                      |
| G3   | Pass. The drag snaps to the 10-minute buckets: 13:00-14:00 UTC gives 15,664. Go to Timestamp lands on 13:00:00.019 UTC (`line_number` 97711).                                |
| G4   | Pass: 4,000 rows (1000-4999) with history, 1,000 (4000-4999) without.                                                                                                        |
| G5   | Pass: 1,000,000 rows about 2 s after launch; peak working set 518 MB; 47,448 and 2,534.                                                                                      |
| G6   | Pass: rows stream in; 5,000 JSON and 2,000 logfmt rows, both parsed.                                                                                                         |
| G7   | Pass. `rename`: the 10,000 retained rows are `line_number` 13950-23949 with no gaps. `copytruncate`: 6,000 of 6,000. `truncate`: 5,886 of 6,000 after 4 truncations.         |
| G8   | Pass: TCP and UDP 1,000 each; two TCP senders give 3,000 rows and 0 errors; logfmt sent 13 s after listener start parses; TLS 100 rows, refused without `--tls-skip-verify`. |
| G9   | Pass with `day.jsonl` moved away: 63,726 rows, timestamp descending; 222,019 after clearing; the GUI export counts 63,726 in `slp`.                                          |
| G10  | Not run for UTC: the GUI has no display-zone setting (GUI finding 2). Local zone: GUI and `slp` both show 2026-03-01 01:00:00.082.                                           |

GUI findings:

1. **Open: Apache error timestamps never parse as time.** `slp validate $fxmt_apache-error.log` reports `timestamp present=2000 matching=0 (type mismatch)`. The GUI shows the raw text, marks the column with a warning,
   and the histogram stays empty. The same happens for `Sun Mar 01 00:00:00 2026` and for six fractional digits, which are the template's own sample
   forms.
1. **Open: the GUI has no display-zone setting.** It always uses the Windows
   zone, so the UTC half of G10 cannot run without changing the system zone.
1. **Minor: `StructuredLogViewer --help` starts the GUI** and restores the
   last session instead of printing usage; the parser defines no help option.
1. **Minor: Preferences opens taller than the screen.** On the display used
   for this run, OK and Cancel are below the bottom edge until the dialog is
   maximized.
1. **Minor: a network listener reads as down while it waits.** Before the
   first client connects, the tab says `Disconnected` and the status bar says
   `Source unavailable`, although the port is listening.
1. **Minor: the Apache access `bytes` column shows a type warning** because
   Apache writes `-` for a zero-byte response.
