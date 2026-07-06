# Continue + local Qwen integration

Glue for driving a local Qwen (MLX) stack from the Continue VS Code extension,
with tardygrada wired in as an MCP fact-grounding server. These files live
outside this repo in normal use; snapshotted here so they're versioned.

| File | Lives at | What it does |
| ---- | -------- | ------------ |
| `qwen-server` | `~/qwen-skills/bin/qwen-server` | Manages the stack: `fast` (mlx_lm.server :8080), `agent` (vllm-mlx :8081), `tardy` daemon, `onto` (open-ontologies grounding socket). `start`/`stop`/`restart`/`status`/`heal`. |
| `continue-config.yaml` | `~/.continue/config.yaml` | Continue models (fast/thinking/agent) + MCP servers: `qwen-subagents`, `web-access`, `filesystem`, `tardygrada`, `brain-in-the-fish`. |
| `mcp-servers/filesystem_tools.py` | `~/.continue/mcp-servers/` | Read-only filesystem MCP: `list_directory`, `find_files`, `read_file` (incl. `.docx`), `grep_files`. Continue's built-in file tools only see the open workspace; this reaches any path. |
| `mcp-servers/tardygrada_bridge.py` | `~/.continue/mcp-servers/` | stdio framing shim: newline-delimited JSON ⇄ Content-Length. `tardy mcp-bridge` speaks LSP framing; Continue speaks newline JSON, so without this tardy's tools never load. (Same as `hooks/targy-mcp-wrapper.py`.) |

## heal

`qwen-server heal` probes every service with a real liveness check and restarts
what's wedged:

- `fast`/`agent` — 1-token generation probe (a bound-but-deadlocked server
  passes a `/v1/models` check but fails real generation).
- `onto` — sends an actual `ground` query over the unix socket; open-ontologies
  can hold the socket open while its query engine is wedged. If dead, it
  restarts onto **and bounces the tardy daemon** so grounding reconnects.

## Gotcha

Never start `fast` and `agent` in the same instant — loading two 35B models
concurrently crashes both (`cannot schedule new futures after interpreter
shutdown`). Start `agent`, wait ~15s, then `fast`.
