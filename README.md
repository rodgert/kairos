# kairos

CLAP plugin host for the [nomos-studio](https://github.com/nomos-studio) platform.
Bridges the [nous](https://github.com/nomos-studio/nous) Clojure compositional
surface to CLAP plugins and hardware MIDI/audio via a Unix-socket IPC protocol.

```
nous (Clojure REPL)
    ↕  EDN over Unix socket (nomos-rt IPC)
kairos (this)
    ↕  CLAP ABI
kairos-grid · third-party CLAP plugins · Faust WASM bridge
```

## Features

- **CLAP host** — discovers, loads, and manages CLAP plugin instances in a
  directed audio graph.  Handles audio ports, CLAP params, and the custom
  param-bus / tap-bus / patch-bus extensions.
- **Ableton Link peer** — beat-sync authority; propagates tempo and transport
  state to the Link session.
- **nomos-rt IPC** — Unix-domain socket accepting EDN-framed control messages
  from nous: graph load/reset, note events, param sets, modulator control,
  txlog session management, and WASM hot-swap.
- **Faust WASM bridge** (optional) — loads Faust-compiled `.wasm` files as
  first-class CLAP plugins via wasmtime.  Supports gapless RCU hot-swap.
- **MIDI / OSC I/O** via RtMidi and a UDP OSC server.

## Build

```bash
# Minimum: just the kairos runtime binary
cmake -B build
cmake --build build

# With tests
cmake -B build -DKAIROS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build

# With Faust WASM bridge (downloads wasmtime pre-built binaries)
cmake -B build -DKAIROS_WASM_BRIDGE=ON
cmake --build build
```

**Requirements:** cmake ≥ 3.20, C++20 compiler, network access for FetchContent
(CLAP SDK 1.2.6, nomos-rt, RtMidi, RtAudio, Ableton Link).

Pass `-DNOMOS_RT_DIR=/path/to/nomos-rt` to use a local nomos-rt checkout.

## Usage

```bash
# Start kairos; connect nous via the default socket
./build/kairos

# Full options
./build/kairos \
  --socket /tmp/kairos.sock \   # IPC socket (default: /tmp/kairos.sock)
  --midi-port 1 \               # RtMidi output port index
  --midi-in-port 0 \            # RtMidi input port index
  --osc-port 9001 \             # UDP OSC listen port (default: 9001)
  --bpm 120 \                   # Initial Link tempo
  --no-audio                    # Disable audio (software-only clock)
```

From the nous REPL:

```clojure
(require '[nous.kairos :as kairos])
(kairos/start-kairos! :binary "/usr/local/bin/kairos")
(kairos/list-plugins!)
(kairos/send-graph-load! my-session-graph)
```

## IPC protocol

Wire format: `[uint32 payload_len][uint8 type][uint8 reserved×3][UTF-8 EDN]`

See [`nomos-rt/include/nomos/rt/ipc.hpp`](https://github.com/nomos-studio/nomos-rt)
for the full message type table (0x30–0x52).

## Architecture

```
kairos/
  src/
    main.cpp                  — entry point, thread wiring
    control_thread.cpp        — CLAP-specific IPC dispatch
    plugin_graph_manager.cpp  — CLAP graph lifecycle
    plugin_instance.cpp       — per-plugin CLAP wrapper
    plugin_discovery.cpp      — filesystem plugin scanning
    wasm_bridge_plugin.cpp    — Faust WASM → CLAP adapter (optional)
  include/kairos/
    clap_kairos_*.h           — custom CLAP extensions (tap/param/patch/hot-swap bus)
```

## License

GPL-2.0-or-later — see [LICENSES/](LICENSES/).  Binaries link Ableton Link
(GPL-2.0-or-later) and nomos-rt; the resulting binary is GPL-2.0-or-later.
