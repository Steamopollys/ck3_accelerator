# accel_decompress_probe (dev-only)

Profiling probe for CK3 save-decompression RE. Not shipped: the `_dev_` prefix marks it as tooling.

It hooks zlib `inflate(z_stream*, int)` in `ck3.exe` (RVA `0x035C4340`) and logs a CSV row around each call: input bytes available, output buffer free, caller, and elapsed time. Rows from a real session answer how CK3 streams a save through zlib.

## Output

One CSV at `<install_dir>\logs\probe_decompress.csv`, where `<install_dir>` is the game's `binaries\` folder. It's appended across launches; each launch first writes a `# session_start <timestamp> module_base=0x...` banner.

| column             | meaning                                                            |
| ------------------ | ------------------------------------------------------------------ |
| `call_index`       | monotonic counter, one per `inflate` call this session            |
| `avail_in_before`  | `z_stream.avail_in` (input bytes available) before the call        |
| `avail_out_before` | `z_stream.avail_out` (free output bytes) before the call           |
| `avail_in_after`   | `avail_in` after (before - after = bytes consumed)                 |
| `avail_out_after`  | `avail_out` after (before - after = bytes produced)                |
| `caller_rva`       | return address as a hex RVA into `ck3.exe`                          |
| `elapsed_us`       | `inflate` wall-clock time, microseconds (QPC)                       |
| `ret_code`         | zlib return code (`0`=Z_OK, `1`=Z_STREAM_END, negatives = errors)  |

## Install

Install the accelerator as described in the root README, then add `plugins\accel_decompress_probe.dll` and enable `accel_decompress_probe` in `config.toml` `[plugins]`. Close CK3 first: it locks the DLLs while running, so `Get-Process ck3*` must print nothing before you copy.

## Run

1. Launch CK3 through Steam. An "untested build" warning is expected; continue.
2. Load two saves in one session for a useful spread: a small/early one (867 or 1066, just started) and a large/late one (long game, many characters/provinces). Loading each to the map is enough; you needn't keep playing.
3. Quit CK3 fully (`Get-Process ck3*` prints nothing), then collect `binaries\logs\probe_decompress.csv` and send it back. The `# session_start` banners mark which save-loads belong to which launch.

## Notes

- If `inflate` isn't found on this build, the probe logs a WARN (`inflate signature not found; probe inert`) and installs no hook: the game runs normally, no rows.
- Dev tool: it flushes the CSV every row and serialises logging with a mutex, so it adds measurable per-call overhead. Don't ship it or leave it on for normal play.
