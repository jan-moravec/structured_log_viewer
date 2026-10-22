# `slp` — Command-Line Reference

`slp` (StructuredLogProcessor) is the companion command-line tool to the Structured Log Viewer GUI. It links `loglib` only, never Qt, and runs the same parsing, filtering, sorting, and export code without a window. It ships next to `StructuredLogViewer` in every package.

## Synopsis

```text
slp [global flags] <command> [command flags] [source]...
```

Global flags are accepted before and after the command.

| Command    | Purpose                                                     |
| ---------- | ----------------------------------------------------------- |
| `query`    | Filter, sort, and export rows.                              |
| `stats`    | Summarise a source by level, time bucket, or column value.  |
| `schema`   | Print the detected columns without emitting rows.           |
| `validate` | Report parse health and exit non-zero on errors.            |
| `bundle`   | Write a `.slvbundle` the GUI opens with its session intact. |
| `formats`  | List parsers and regex templates.                           |
| `tail`     | Follow a file, stdin, or a `tcp://` / `udp://` listener.    |

## Sources

A source is one of:

- a log file path, optionally compressed (`.gz`, `.bz2`, `.xz`, `.zst`);
- `-` for standard input (at most once);
- a `.slvbundle`;
- `tcp://[host]:port` or `udp://[host]:port` — only for `tail`, which binds a listener. Every other command rejects them and points at `slp tail`.

Several sources concatenate in the order given. Rotated siblings of a file (`app.log.1`, `app.log.2.gz`, dated names) load oldest first with the named file last, unless `--no-rotation-history` is set. Use `--sort` on a time column when the output must be chronological.

## Global flags

| Flag                          | Default                 | Meaning                                                                        |
| ----------------------------- | ----------------------- | ------------------------------------------------------------------------------ |
| `-q`, `--quiet`               | off                     | Suppress warnings and progress. Errors still print.                            |
| `-v`, `--verbose`             | off                     | Per-stage timing and row counts on stderr. Repeatable.                         |
| `--color auto\|always\|never` | `auto`                  | Colour human-readable text only. `NO_COLOR` (any value) forces `never`.        |
| `--threads N`                 | `0`                     | Parser threads. `0` is the library default.                                    |
| `--tz ZONE`                   | `local`                 | Zone for naive timestamps and rendered times: an IANA name, `local`, or `UTC`. |
| `--no-rotation-history`       | off                     | Skip rotated-sibling discovery.                                                |
| `--template-dir DIR`          | platform user directory | Directory of user regex templates. Overrides `SLV_TEMPLATE_DIR`.               |
| `-h`, `--help`                |                         | Help. `--help-advanced` adds `--batch-size`.                                   |
| `--version`                   |                         | Print the `slp` and `loglib` versions.                                         |

If no `tzdata` directory is found next to the executable (or under `$APPDIR` on Linux AppImages), `slp` warns once and uses UTC.

## Parser flags (all commands except `formats`)

| Flag                    | Default | Meaning                                                        |
| ----------------------- | ------- | -------------------------------------------------------------- |
| `-f`, `--format`        | `auto`  | `auto`, `json`, `logfmt`, `csv`, or `regex`.                   |
| `--regex PATTERN`       |         | PCRE2 pattern with named captures. Implies `--format regex`.   |
| `--regex-template SLUG` |         | Template from `slp formats`. Implies `--format regex`.         |
| `--no-multiline-logfmt` | off     | Parse each logfmt line alone instead of joining continuations. |
| `--batch-size BYTES`    | library | Advanced. Accepts suffixes such as `1MiB`.                     |

## Row-selection flags (`query`, `stats`, `bundle`, `tail`)

| Flag                | Default | Meaning                                                                                      |
| ------------------- | ------- | -------------------------------------------------------------------------------------------- |
| `--filter QUERY`    | none    | GUI filter syntax. Repeat to AND in order.                                                   |
| `--since TIME`      | none    | At or after this time on the first time column. ISO-8601, `now`, or relative such as `-30m`. |
| `--until TIME`      | none    | At or before this time. Not accepted by `tail`.                                              |
| `--min-level LEVEL` | all     | `trace`, `debug`, `info`, `warn`, `error`, or `fatal`.                                       |
| `-x`, `--invert`    | off     | Keep the rows the combined filter rejects.                                                   |

## `slp query`

Filters, sorts, and exports rows.

| Flag                        | Default      | Meaning                                          |
| --------------------------- | ------------ | ------------------------------------------------ |
| `--sort COLUMN[:asc\|desc]` | source order | Sort by one column.                              |
| `--columns A,B,C`           | visible set  | Columns to emit, in that order.                  |
| `--all-columns`             | off          | Emit every column.                               |
| `--limit N`                 | none         | At most N rows after filtering and sorting.      |
| `--offset N`                | 0            | Skip the first N rows. Exclusive with `--last`.  |
| `--last N`                  | none         | Only the last N rows. Exclusive with `--offset`. |
| `-o`, `--output PATH`       | `-`          | Destination, or `-` for stdout.                  |
| `-t`, `--output-format`     | `jsonl`      | `jsonl`, `csv`, `markdown`, or `snapshot`.       |
| `--no-header`               | off          | Omit the header for `csv` and `markdown`.        |
| `--json-visible-only`       | off          | Honour `--columns` in JSON Lines.                |
| `--force`                   | off          | Overwrite an existing `--output`.                |
| `--count`                   | off          | Print the matching row count only.               |
| `--allow-empty`             | off          | Exit 0 when nothing matches.                     |
| `--strict`                  | off          | Exit 4 when the parse reported errors.           |
| `--config PATH`             | none         | Session or columns JSON applied before flags.    |
| `--save-session PATH`       | none         | Write the effective full session after the run.  |
| `--save-columns PATH`       | none         | Write columns and highlight rules only.          |

## `slp stats`

| Flag                              | Default               | Meaning                                     |
| --------------------------------- | --------------------- | ------------------------------------------- |
| `--by level\|time\|column`        | `level`               | Summary axis.                               |
| `--bucket WIDTH`                  | `auto`                | Histogram width for `--by time`.            |
| `--max-buckets N`                 | 60 text, 500 json/csv | Bucket budget.                              |
| `--top N`                         | 20                    | Distinct values printed by `--by column`.   |
| `--column NAME`                   |                       | Column for `--by column`. Required with it. |
| `--output-format text\|json\|csv` | `text`                | Rendering.                                  |

## `slp schema`

| Flag                         | Default | Meaning                                 |
| ---------------------------- | ------- | --------------------------------------- |
| `--sample N`                 | 3       | Sample values per column. `0` disables. |
| `--output-format text\|json` | `text`  | Rendering.                              |

Exit 3 when the source yields no columns.

## `slp validate`

| Flag                         | Default         | Meaning                                             |
| ---------------------------- | --------------- | --------------------------------------------------- |
| `--max-errors N`             | 20              | Parse errors printed in text mode.                  |
| `--max-error-rate PERCENT`   | any error fails | Exit 0 while the error rate stays at or below this. |
| `--output-format text\|json` | `text`          | Rendering.                                          |

`--strict` is not accepted; validate is strict by default.

## `slp bundle`

Writes a `.slvbundle`. By default the bundle is **non-destructive**: every row is kept, and the effective filter, sort, and column set are stored in the embedded configuration for the GUI to apply. `--filtered-rows` is the destructive variant.

| Flag                        | Default     | Meaning                                                                           |
| --------------------------- | ----------- | --------------------------------------------------------------------------------- |
| `-o`, `--output PATH`       | required    | Destination. A missing `.slvbundle` extension is appended. Not `-`.               |
| `--filtered-rows`           | off         | Write only matching rows and clear the embedded filter.                           |
| `--sort COLUMN[:asc\|desc]` | none        | Sort stored in the bundle.                                                        |
| `--columns A,B,C`           | visible set | Column set stored in the bundle.                                                  |
| `--config PATH`             | none        | Session JSON that seeds the embedded configuration (anchors, highlight rules, …). |
| `--compression-level N`     | 3           | zstd level.                                                                       |
| `--bundle-threads N`        | 0           | zstd workers. `0` is single-threaded.                                             |

The destination is replaced atomically; a failed or cancelled run leaves no partial file. On a terminal, progress shows as one rewritten stderr line unless `--quiet`.

## `slp formats`

Lists parsers and regex templates. No source is read.

| Flag                         | Default | Meaning                                                     |
| ---------------------------- | ------- | ----------------------------------------------------------- |
| `--show SLUG`                |         | Print one template as JSON. Unknown slugs exit 2.           |
| `--user`                     | off     | Also list user templates and the built-in each one shadows. |
| `--output-format text\|json` | `text`  | Rendering.                                                  |

## `slp tail`

Follows **one** source and emits rows as they arrive.

- A file is pre-filled with its last `--retention` rows, then followed. Rotation (rename, copy-truncate, truncate, delete-and-recreate) is handled by the library.
- `-` reads standard input to EOF.
- `tcp://[host]:port` and `udp://[host]:port` bind a listener (all interfaces when the host is omitted) and print `slp tail: listening on <uri>` on stderr once bound. TLS is not supported: a TLS-only TCP peer produces unparseable bytes, and `slp tail` says so.

| Flag                                | Default     | Meaning                                                                                                                                                     |
| ----------------------------------- | ----------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `--columns A,B,C`                   | visible set | Columns to emit.                                                                                                                                            |
| `--output-format auto\|jsonl\|text` | `auto`      | `auto` is `text` when stdout is a terminal and `jsonl` otherwise. `text` is `<time> <level> <message> key=value…`, coloured by level when colour is active. |
| `-o`, `--output PATH`               | stdout      | Append rows to PATH. Not atomic; the file is never replaced.                                                                                                |
| `--max-rows N`                      | none        | Stop after N emitted rows, exit 0.                                                                                                                          |
| `--for DURATION`                    | none        | Stop after `30s`, `5m`, `1h`, …, exit 0.                                                                                                                    |
| `--retention N`                     | 10000       | Rows kept in memory. `0` is unbounded; otherwise 1000–1000000.                                                                                              |
| `--strict`                          | off         | Exit 4 when any line failed to parse.                                                                                                                       |

Output is flushed once per batch. Filters apply per arriving batch. Rows are emitted before retention can evict them, so the cap never drops output. `--filter`, `--since`, `--min-level`, and `--columns` that name a column the stream has not shown yet hold rows back until it appears; the run fails with that flag's message at end of stream, or once 10 000 rows are waiting.

`--max-rows`, `--for`, end of standard input, and `SIGINT` / `Ctrl+C` all exit 0. A second signal terminates immediately.

## Exit codes

| Code | Meaning                                                                                                    |
| ---- | ---------------------------------------------------------------------------------------------------------- |
| 0    | Success.                                                                                                   |
| 1    | The command ran but the result was empty. `--allow-empty` turns this into 0.                               |
| 2    | Usage error: unknown flag, bad value, malformed filter, unresolvable column.                               |
| 3    | Input error: source missing, unreadable, undetectable, failed to decompress, or a listener could not bind. |
| 4    | Data error: parse errors under `--strict`, or `validate` found errors.                                     |
| 5    | Output error: destination unwritable, exists without `--force`, or disk full.                              |
| 70   | Internal error: an unexpected failure inside `slp`. Please report it.                                      |
| 130  | Cancelled by `SIGINT` (every command except `tail`, which exits 0).                                        |

A closed stdout pipe (`slp query … | head`) ends the run quietly with exit 0.

Warnings, verbose stage lines, and the parse-error summary go to stderr as they happen. `--quiet` drops warnings and the parse-error summary; errors always print.

## Pipeline order

`query` runs these stages in order: **parse** every source → **filter** (`--filter`, `--since`/`--until`, `--min-level`, then `--invert` over the combination) → **sort** → **offset / last / limit** → **project** columns → **export**.

## Precedence chain

For session state (filter, sort, columns), later layers override earlier ones:

1. the configuration embedded in a `.slvbundle` source;
1. `--config PATH`, which replaces the bundle's configuration (its `source` is ignored);
1. individual flags: `--filter`, `--since`, `--until`, `--min-level`, `--invert`, `--sort`, `--columns`, `--all-columns`.

Any row-selection flag replaces the configuration's filter as a whole. `--all-columns` beats `--columns`, which beats the configuration's column visibility.

## Recipes

**CI gate — fail the build when the log has errors:**

```sh
slp validate --max-error-rate 0.1 build.log
slp query --min-level error --count build.log   # prints the count; exit 1 when it is 0
```

**Incident extraction — one hour of errors as JSON Lines, then a bundle for the GUI:**

```sh
slp query --since 2024-05-01T14:00:00Z --until 2024-05-01T15:00:00Z \
          --min-level error --sort ts -o incident.jsonl app.log
slp bundle --min-level error --sort ts -o incident app.log   # writes incident.slvbundle
```

**Format pre-flight — check what `slp` will see before building a pipeline:**

```sh
slp schema app.log
slp validate app.log
slp formats --user
```

**Pipe to `jq`:**

```sh
slp query --filter 'level:error' app.log | jq -r '.msg'
slp tail --min-level warn tcp://0.0.0.0:5140 | jq -c 'select(.service == "db")'
```
