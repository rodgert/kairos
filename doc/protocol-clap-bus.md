<!--
SPDX-FileCopyrightText: 2025-2026 nomos-studio contributors

SPDX-License-Identifier: GPL-3.0-or-later
-->

# kairos CLAP bus extensions

**Owning component:** `kairos` — the CLAP host. This document specifies the three
custom CLAP extensions kairos defines for block-rate communication with the CLAP
plugins it hosts (principally `kairos-grid`).

**Normative source of truth:** the three plain-C headers under `include/kairos/`:
`clap_kairos_param_bus.h`, `clap_kairos_tap_bus.h`, `clap_kairos_patch_bus.h`. Their
struct/vtable definitions and protocol comments are authoritative; this document
explains the shared pattern and the three buses' roles.

## Purpose

kairos hosts sample-rate DSP plugins (the `kairos-grid` engine, Surge XT, …) via CLAP.
Standard CLAP covers audio and parameter automation, but nomos-studio needs three
additional block-rate channels between host and plugin: pushing a curated parameter
frame in, reading a performance-tap frame out, and swapping the plugin's internal graph
from an EDN descriptor. These are expressed as **custom CLAP extensions** rather than a
side protocol, so they ride CLAP's own lifecycle and stay arms-length: the host and the
plugin agree only on a small C ABI (a header), not a shared build.

## Boundary parties

| Side | Who | Role |
|---|---|---|
| Host / consumer | `kairos` (CLAP host) | queries the extension, drives param/patch, reads taps |
| Plugin / provider | `kairos-grid` (and any kairos plugin that opts in) | implements the extension vtable |

## Transport & mechanism

CLAP custom extensions. After `create_plugin()`, the host calls
`get_extension(plugin, <ext-id>)`; a non-null result is the plugin's vtable for that
bus. All three follow the same shape:

- The extension id is a string (`"kairos/param-bus"`, etc.).
- The vtable is a `struct` of `CLAP_ABI` function pointers.
- Schema-carrying buses expose `get_schema()` returning a stable snapshot with an
  `epoch` counter; the host re-queries on any epoch change (an `activate()` / `reset()`
  invalidates the schema).
- Frame calls run on the **audio thread**, before/around `process()`.

The headers are **plain C, shared verbatim** between the `kairos` and `kairos-grid`
source trees (copied into both; the ABI is stable within a major version) — this keeps
the two repos decoupled at build time. Keep the copies in sync.

## The three buses

| Extension id | Header | Direction | Rate | Role |
|---|---|---|---|---|
| `kairos/param-bus` | `clap_kairos_param_bus.h` | host → plugin | block | push the curated parameter frame into the engine |
| `kairos/tap-bus` | `clap_kairos_tap_bus.h` | plugin → host | block | read the performance-tap frame out of the engine |
| `kairos/patch-bus` | `clap_kairos_patch_bus.h` | host → plugin | on change | push an EDN patch descriptor; the engine atomically rebuilds |

### `kairos/param-bus` — control in
`get_schema()` → `{epoch, count, entries[]}` where each entry is `{id, name}`
(e.g. `"env/tempo_hz"`). `set_param_frame(values, count)` writes `values[i]` into the
port named by `entries[i]`, once per block before `process()`. CLAP parameter-automation
events inside `process()` take precedence for the same port (DAW automation wins).

### `kairos/tap-bus` — telemetry out
Symmetric counterpart to param-bus, opposite direction. `get_schema()` → the tap port
schema; `get_tap_frame(&out_count)` → the current per-block tap values the host reads for
performance monitoring / scope matter.

### `kairos/patch-bus` — structural rebuild
`push_patch(edn_descriptor, …)` hands the plugin an EDN patch string; the plugin
atomically rebuilds its GridEngine at the next `process()` block boundary, invalidating
and re-publishing the param-bus and tap-bus schemas as part of the same swap. `get_patch()`
returns the current descriptor.

## Stability & versioning

The extension ids and vtable layouts are the ABI; stable within a major version. The
`epoch` counter is the in-band schema-version signal. Because the header is copied into
both repos, the header revision is the shared contract — bump deliberately and update
both copies together.

## Reimplementing a plugin

A CLAP plugin joins these buses by returning the corresponding vtable from
`get_extension()` and honouring the thread/lifecycle rules in the header. No kairos code
is linked — the header ABI is the whole contract. `kairos-grid` is the reference provider.

## Related

- Product boundary index: `../../nomos-studio/doc/component-boundaries.md`
- Sibling protocols: nomos-rt IPC (`../../nomos-rt/doc/protocol-ipc.md`), NousPort/BEAM
  (`../../nomos_beam/guides/protocol-nousport.md`).
