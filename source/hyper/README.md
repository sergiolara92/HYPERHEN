# Hyper worker (R52)

This is the local speech-to-text, dialogue and speech-synthesis worker embedded
in HyperHEN 0.9.5. `runtime/` contains the R52 first-party sources (worker
header includes are relative to the published include root) and
`streaming/` contains its reusable 512 MiB weight arena. Model weights remain
external in `/data/Hyper`; no model is included in this source tree or the ELF.

Qwen2.5 0.5B Q4_K_M is the recommended, console-confirmed starting point.
Qwen2.5 7B Q5_K_M completed one Italian microphone-to-speaker turn on FW 10.01
in R51, taking about 138 seconds overall. The R52 worker retains that reply
policy and conversation-prefix reuse, and optimizes CPU callbacks and weight
access. There is no new console latency measurement for these optimizations.
Other unconfirmed models remain beta; 7B is experimental and very slow.
Compilation and host tests do not establish usable real-time performance.
Inference uses the CPU.

Disable Hyper and wait for its model to unload before demanding games or
plugin tests. A resident 7B model was associated with a GTA V black screen
with audio in a reported test; the game opened with Hyper disabled. The
512 MiB weight arena is only part of the assistant's memory use. Concurrent
7B/game operation is not a validated configuration.

## Build

The ordinary `scripts/build_release.sh` preserves the checksum-pinned R52 worker.
To rebuild that worker separately with the same retained seven AI libraries:

```bash
export PS5_PAYLOAD_SDK=/absolute/path/to/ps5-payload-sdk
python3 scripts/build_hyper_worker.py --check-only
python3 scripts/build_hyper_worker.py --jobs 4
```

The versioned build-input ZIP must be extracted first. The recipe verifies the
AI libraries and the isolated CRT and links that CRT last. It never modifies
the installed SDK. The output is `build-hyper-worker/Hyper-Assistant-R52.elf`.
The recipe is relocatable; no original development workspace is required.

## AI source and provenance

`upstream-lock.json` records the exact llama.cpp, whisper.cpp and eSpeak NG
source archive URLs, revisions and SHA256 values. The corresponding complete
source snapshot is included in the build-input asset as
`release-inputs/hyper-ai/upstream-source.tar.gz`. Licenses and the headers used
by this adapter are in `third_party/hyper-ai/`.

`patches/llama-model-loader.cpp` and `patches/ggml-cpu.cpp` are the adapted
translation units used by the retained AI libraries. They provide bounded
weight loading and execution for the 7B path. The loader is compiled with
`HYPER_PS5_SERIAL_TENSOR_VALIDATION=1`; the other patched unit uses the upstream
CPU target flags and includes `streaming/hyper_weight_stream.h`. The fixed arena
provider itself is linked into the worker. These sources accompany the frozen
library inputs; the optional worker recipe does not rebuild the entire AI stack.

The speech, dialogue and voice engines are upstream projects, credited under
their original licenses. Hyper's integration, data installer, model selection,
session lifecycle and DualSense bridge are separate project components.
