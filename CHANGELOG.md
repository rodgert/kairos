# Changelog

All notable changes to kairos are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

---

## [Unreleased]

---

## [0.1.0] — 2026-06-07

### Added

#### Core CLAP hosting

- **Plugin hosting layer** — `plugin_instance`, `plugin_graph_manager`,
  `plugin_discovery`; loads CLAP plugins from `.clap` bundles via `dlopen`
  and the CLAP factory ABI. Manages plugin lifecycle (init → activate →
  process → deactivate → destroy).
- **Audio engine** — RtAudio-backed stereo I/O; routes host audio through
  the plugin graph via `process_thread`.
- **MIDI I/O** — RtMidi-backed output and input via `nomos::rt::midi_io`;
  input events are injected into the CLAP event queue.
- **Plugin graph manager** — directed audio graph with add/remove/connect
  operations; calls `process()` in topological order each audio block.

#### Custom CLAP extensions (host-side consumers)

- **`kairos/tap-bus`** — host reads block-rate signal taps from plugins for
  modulation routing and scope display.
- **`kairos/param-bus`** — host writes named float parameters to plugins
  without CLAP automation overhead.
- **`kairos/patch-bus`** — host pushes EDN graph descriptors to
  kairos-grid; plugin swaps engine atomically at next block boundary.
- **`kairos/hot-swap/2`** — gapless WASM DSP module replacement.
  `request(new_path, old_path)` — `old_path` identifies which slot to
  replace in multi-module patches (NULL = first slot).

#### Faust WASM bridge (optional)

- **`wasm_bridge_plugin`** — presents Faust-compiled `.wasm` files as
  first-class CLAP plugins via wasmtime.  RCU gapless hot-swap: builds
  new DSP state on the control thread, publishes atomically, drains
  in-flight audio readers via `synchronize_rcu()`.
- **Math imports** — `WasmModuleCache` holds a `wasmtime_linker_t`
  pre-populated with 19 Faust math imports (`env._sinf`, `env._cosf`,
  etc.); any Faust 2.80+ patch instantiates without modification.

#### nomos-rt IPC (control plane)

- Unix-domain socket IPC server accepting EDN-framed control messages from
  nous/aion.  Handled message types:
  - Session: `SESSION-OPEN/CLOSE`, `REGISTER-SOURCE`, `TX-LOG`
  - Graph: `GRAPH-LOAD`, `GRAPH-RESET`, `PLUGIN-LIST-REQ/RESP`
  - MIDI: `NOTE-ON/OFF`, `MIDI-IN`, `CC`, `PITCH-BEND`, `CHAN-PRESSURE`,
    `SYSEX`, `MTS`
  - Modulators: `MODULATOR-START/STOP/UPDATE`
  - Scheduler: `SCHEDULE-BUNDLE` — beat-accurate multi-event dispatch
  - Transport: `LINK-SET-TEMPO`, `LINK-START/STOP-TRANSPORT`
  - WASM: `WASM-HOT-SWAP` — routes to `hot_swap_node(new_path, old_path)`
  - MSG-TICK (0x50) pushed to clients at 24 PPQN with beat + modulator outputs.

#### Ableton Link

- `link_peer` — joins the Link session as a beat-sync authority; tempo
  and transport state propagated to kairos clients.

#### Public extension headers

- `include/kairos/clap_kairos_hot_swap.h` — `CLAP_EXT_KAIROS_HOT_SWAP`
  (`"org.cljseq.kairos.ext.hot-swap/2"`) — shared between kairos and
  kairos-grid; keeps both implementations in sync.
- `include/kairos/clap_kairos_{param,tap,patch}_bus.h` — custom extension
  ABI headers.

### Fixed

- `read_message` now returns `eof` (not `io_error`) on clean peer
  disconnect, preventing spurious error logs on nous shutdown.

### Changed

- `cljseq-rt` renamed to `nomos-rt`; namespace updated to `nomos::rt`
  throughout.
- nomos-rt dependency pinned to `0d7ef76` (msg_route_set + msg_midi_event)
  for reproducible builds.
