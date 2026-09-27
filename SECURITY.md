# Security

> **[中文](./SECURITY.zh.md) | English**

## What this repo builds

`uia_agent.exe` is a pure command-line tool: it reads arguments on stdin,
operates on Windows desktop UI (enumerate / snapshot / find / click / set text /
scroll / drag / swipe / screenshot / window state), and prints one UTF-8 JSON
line on stdout. It has **no network access, no persistence, no self-update**.
It runs with the privileges of whatever process launched it.

## Supply-chain integrity

- The release workflow (`.github/workflows/release.yml`) builds the exe on
  GitHub-hosted Windows runners from the committed source, then:
  1. `scripts/pack.ps1` encodes the exe to `assets/uia_agent.exe.b64` and writes
     its SHA-256 to `assets/uia_agent.exe.sha256`
  2. `scripts/verify.ps1` round-trips the b64 back and asserts the hash matches
  3. both asset files are committed back to `master`
  4. the exe is attached to the matching GitHub release
- Anyone can independently audit the binary: download the release exe, compute
  `Get-FileHash -Algorithm SHA256`, and compare against
  `assets/uia_agent.exe.sha256` in the repo at the same tag.

## Capabilities and their risk

| Capability | Risk |
|---|---|
| list / snapshot / find / get_text / screenshot | read-only: enumerates UI structure and pixels |
| minimize / maximize / restore / topmost / close | changes window state of a target window |
| click / set_text / scroll / drag / swipe | **simulated input**: sends mouse/keyboard events into the desktop session |

Higher-risk commands are gated by the consuming plugin (see
`dsh-uia-agent`'s SECURITY) through user approval — this repo's exe itself does
**not** enforce permissions; enforcement belongs to the caller.

## Responsibility split

- **This repo** is responsible for: correct and safe implementation of the
  automation operations, reproducible builds, and committed sha256 assets.
- **The consuming plugin** (`cnyc6n/dsh-uia-agent`) is responsible for
  permission tiers and routing high-risk calls through user approval.
- **The user** decides whether to approve elevated (workspace-write /
  danger-full-access) operations. Approving `click`/`set_text`/`drag` means
  accepting that real input will be injected into the desktop session.

## Boundaries / limitations

- Windows UIPI blocks non-elevated processes from accessing elevated (admin)
  windows; this tool detects that and reports `elevated_requires_admin` rather
  than returning a misleading empty tree.
- Running the exe as administrator grants it the ability to interact with
  elevated windows; only run it that way if you trust the caller.

## Reporting

For security issues, open a private advisory or contact the maintainer via a
GitHub issue marked `security`. Do not open public issues with exploit details
before a fix is available.
