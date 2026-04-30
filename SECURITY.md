# Security Policy

## Reporting a Vulnerability

If you believe you have found a security vulnerability in Tardygrada — anything
from a memory-safety bug in the C runtime to a credential-handling issue in the
optional Anthropic API path — please report it privately, not in a public issue.

**Preferred channel:** open a private security advisory on GitHub:
<https://github.com/fabio-rovai/tardygrada/security/advisories/new>

If you cannot use GitHub advisories, email the maintainer directly with
`SECURITY` in the subject line. The maintainer is Fabio Rovai, listed in the
repository profile.

Please include:

- A description of the issue and its impact.
- Steps to reproduce, ideally a minimal proof-of-concept.
- Affected version(s) and platform.
- Whether the issue is publicly known (e.g. discussed elsewhere).

I will acknowledge receipt within 72 hours and aim to publish a fix or
mitigation within 14 days for confirmed high-severity issues. This is a
single-maintainer project; please be patient on response time.

## Scope

The following are in scope:

- The C runtime (`src/`) — memory safety, undefined behaviour, race
  conditions in the daemon.
- The MCP bridge (`src/mcp_bridge.c`, `src/mcp/server.c`) — JSON parsing,
  protocol-level injection, header smuggling.
- The daemon (`src/daemon.c`, `src/daemon_client.c`) — Unix socket
  permissions, denial of service, authentication boundaries.
- The optional Anthropic integration (`TARDY_LLM_DECOMPOSE=1`) — credential
  handling, request body construction.
- The compiler / executor for `.tardy` programs (`src/compiler/exec.c`) —
  arbitrary code execution surface.

Out of scope:

- The Coq proofs in `proofs/` — these are public, formally verified, and not
  a security boundary.
- The benchmarks under `evaluation/` — they read public datasets and produce
  numbers; correctness issues there are non-security bugs.
- Performance regressions and detection-quality issues — please open a
  regular issue for those.

## Supported Versions

| Version | Supported          |
| ------- | ------------------ |
| 2.0.x   | :white_check_mark: |
| < 2.0   | :x:                |

## Known Trust Boundaries

The runtime makes the following trust assumptions. If your threat model
violates them, additional sandboxing is required:

1. **`.tardy` source is trusted.** The compiler executes shell commands via
   `exec(...)` blocks defined in `.tardy` programs. A program that contains
   `exec("rm -rf ~")` will run that shell command. Loading a `.tardy` file
   from an untrusted source is equivalent to running an untrusted shell
   script. Future versions may add an opt-in sandbox; today there is none.
2. **The daemon is single-user.** The Unix socket is `0600` (owner-only).
   Multi-user shared boxes should run separate daemons per user, not one
   shared daemon. There is no per-message authentication beyond Unix
   credentials.
3. **The `TARDY_LLM_DECOMPOSE=1` path requires an Anthropic API key.** That
   key is written to a `0600` temp file under `/tmp` for the duration of one
   `curl` invocation, then unlinked. On systems where `/tmp` is shared
   between privilege levels (uncommon), or on misconfigured boxes where
   `/tmp` is sticky-bit-disabled, that file could be readable for the
   millisecond between `mkstemp` and `fchmod`. Air-gap the key or run on
   per-user `/tmp` if this matters.

## v2.0 Hardening Summary

The v2.0 release closed several findings from an internal audit. See
[CHANGELOG.md](CHANGELOG.md) for the full list. Highlights:

- Daemon socket created with `0600` (was world-readable via default umask).
- Daemon recv/send timeouts (5 s) prevent slow-loris DoS.
- `tardy_mcp_server_t` mmap leak on error paths fixed.
- MCP `arguments` parsing no longer truncates JSON containing `}` inside
  string values.
- Optional Anthropic API key removed from `argv` (now passed via curl
  config file at mode `0600`, unlinked after the call).
- User claim is JSON-escaped before being embedded in the Anthropic
  request body (closes a quote/backslash injection).
- Build flags: `-fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE`.
  Linux additionally `-pie -Wl,-z,relro,-z,now`.

## Cryptographic Primitives

- **Hashing:** SHA-256 via [Monocypher](https://monocypher.org/) (vendored
  in `src/vm/monocypher.c`).
- **Signatures:** Ed25519 via Monocypher.
- **No transport encryption** is built in — the daemon runs over Unix
  sockets. If you need a remote daemon, terminate TLS in front of it (e.g.
  via `socat` + a stunnel pair); the daemon protocol does not authenticate
  or encrypt by itself.

## Coordinated Disclosure

I follow standard 90-day coordinated disclosure: a private fix and CVE
(if applicable) get up to 90 days from initial report before a public
write-up. Earlier release is fine if the fix is trivial and there is no
ongoing risk.
