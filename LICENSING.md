# Licensing — the permanent record

This file records a decision that has already been made. It is not a discussion document.
If you are about to add a dependency, read the DO-NOT-LINK list first.

---

## THE RULE

**The app binary must contain ZERO copyleft code.**

The project ships under **JUCE 9's free "Starter" tier**. That EULA forbids causing the
framework to become subject to any open-source licence. So linking GPL/AGPL code into this
app does not merely relicense *our* code — it **breaches the JUCE licence itself** and
pushes the whole project off the Starter tier and onto AGPLv3. That is a materially worse
outcome than "we had to open-source a hobby project", which is why the list below is a hard
rule and not a preference.

**Algorithms and numeric constants are facts and may be reimplemented from their published
description. Source code may not be copied, pasted, transcribed, or machine-translated.**

---

## DO NOT LINK — ever, in any target, in any form

| Project | Licence | Why it is on this list |
|---|---|---|
| **aubio** | GPL-3.0-or-later | **The #1 practical contamination risk.** Best docs, easiest C API, most tutorials — it is the thing you will be tempted by at 1am. Forces the entire app to GPLv3 and off JUCE's Starter tier. Read for education only. |
| **pYIN / Vamp** (Mauch, QMUL) | GPL-2.0-or-later | Same analysis. GPL-2.0-**only** code is additionally incompatible with GPL-3 dependencies. |
| **Essentia** | AGPL-3.0 | Strongest copyleft of the set. Also far too heavy for the audio thread. |
| **madmom** | Non-commercial restricted (+ won't build on modern Windows) | Not a licence we can accept at all. Port the algorithm from its description instead of taking the dependency. |
| **chowdsp_utils** GPL modules — `chowdsp_filters`, `chowdsp_dsp_utils`, `chowdsp_gui`, `chowdsp_visualizers`, … | GPL-3.0 | A trap: it looks like the obvious permissive JUCE helper library. Only `chowdsp_core`, `chowdsp_buffers`, `chowdsp_math`, `chowdsp_simd`, `chowdsp_data_structures` are BSD. **And it contains no pitch detector anyway**, so there is no reason to reach for it. |
| **UltraStar Deluxe (USDX)** | GPL-2.0 | Reimplement. The beat-conversion formula and tolerance constants are facts. |
| **Vocaluxe** | GPL-3.0 | Same. |
| **Performous** | "Other/NOASSERTION" per GitHub; historically GPL-2.0+, mixed third-party assets | **The most valuable reference in the project** (`notes.cc`, `notegraph.cc`, `dynamicnotegraphscaler.cc`). Reimplement from described behaviour. Do not copy source. Unclear licensing makes it worse, not better. |
| **YARG / YARG.Core** | LGPL-3.0 | Published tolerance numbers are facts and freely usable. Do not link. |
| **TarsosDSP** | GPL, and Java | Unusable in C++ regardless. Clean YIN/MPM reading reference only. |

### Safe to lift code from

| Project | Licence | |
|---|---|---|
| **UltraStar Play** | MIT | Scoring structure, joker rule, rating bands, CAMD analyzer design. |
| **UltraSinger** | MIT | Also the architectural reference for the offline pipeline. |
| **GuitarGame_ChartFormats** (Rock Band MIDI vocals spec) | CC0-1.0 | Public domain spec text. |
| **sevagh/pitch-detection** | MIT | Reference for the MPM/NSDF implementation. |

---

## Shipped in the app binary

| Dependency | Version | Licence | Notes |
|---|---|---|---|
| **JUCE** | 9.0.0 (2026-07-21) | **AGPLv3 OR commercial** | Dual-licensed. We take the free **Starter** tier — perpetual, $0, covers revenue/funding up to **$20,000/yr**, no splash-screen requirement (dropped in JUCE 8). Requires registering a juce.com account and accepting the JUCE 9 EULA. **An unregistered `git clone` is AGPL by default.** https://juce.com/legal/juce-9-licence/ |
| lunasvg | bundled (JUCE 9 dependency) | MIT | JUCE 9's new SVG parser. No obligation. |
| **ASIO SDK** | bundled in JUCE since 8.0.11 | **GPLv3 or Steinberg proprietary** | **The one live copyleft edge.** Compiled only when `JUCE_ASIO=1`, i.e. only via `-DVT_ENABLE_ASIO=ON`. For a personal, undistributed tool, GPLv3 obligations never trigger (no distribution = no copyleft). **If the app is ever distributed closed-source**, either build with `VT_ENABLE_ASIO=OFF` (falls back to WASAPI) or obtain Steinberg's proprietary ASIO licence. **Default is OFF.** |
| MPM / NSDF pitch detector | — | **Our own code** | Written from McLeod & Wyvill (2005), referencing `sevagh/pitch-detection` (MIT). Reading and adapting MIT code is unambiguously safe. |
| 1-Euro filter | — | **Our own code** | ~30 lines written from the CHI 2012 paper. Sidesteps any vendored-implementation licence question. |
| Okabe-Ito palette | — | n/a (facts) | Hex values are not copyrightable. |
| Scoring constants | — | n/a (facts) | Tolerance numbers from Performous / YARG / USDX are facts and freely reimplementable. **Do not paste their code.** |

---

## Offline Python pipeline only — never linked into the app

Separate process, separate address space, no contamination path to the binary.

| Package | Version | Licence |
|---|---|---|
| `audio-separator` | 0.44.5 | MIT |
| BS-Roformer / UVR checkpoints | — | **Varied and often unstated.** Use locally; do not redistribute the weights. |
| `swift-f0` | 0.1.2 | MIT |
| `stable-ts` | 2.19.1 | MIT |
| `openai-whisper` (stable-ts dep) | ≤ 20250625 | MIT |
| `whisperx` (fallback only) | 3.8.6 | **BSD variant — verify at the repo before relying on it.** Also pulls `pyannote-audio`, whose gated models require accepting HuggingFace terms. |
| `pyphen` | 0.17.2 | **Tri-licensed GPL-2.0+ / LGPL-2.1+ / MPL-1.1.** Choose LGPL or MPL. Offline-only. |
| `cmudict` | 1.1.3 | BSD-style (CMU), permissive |
| `librosa` | 0.11.0 | ISC |
| `pretty-midi` | 0.2.11.post0 | MIT |
| `music21` | 10.5.0 | BSD-3-Clause |
| `torch` / `torchaudio` / `torchvision` | 2.8.0 / 2.8.0 / 0.23.0 | BSD-3-Clause |
| `onnx2torch-py313` (audio-separator dep) | 1.6.0 | Apache-2.0 |
| `onnxruntime` / `onnxruntime-gpu` | — | MIT |
| `numpy` / `scipy` | — | BSD-3-Clause |
| `mir_eval` | ≥ 0.8 | MIT |
| `demucs` (fallback separator) | 4.1.0 | MIT |

**Two venvs, and the split is deliberate.** `.venv` is the analysis environment and is
**ONNX-only** — `swift-f0` needs no `torch`, and `faster-whisper` covers lyrics without it
(see DESIGN 11.9). `torch` must not be added there. `.venv-separate` holds `torch` and
`audio-separator` for stage 0 and is reached by subprocess. Both are separate processes from
the app, so neither affects the binary's licensing.

**The model weights are the one thing to be careful with.** `audio-separator` is MIT, but the
BS-Roformer checkpoint it downloads is licensed vaguely or not at all — like most of the UVR
model ecosystem. Downloading and running it locally is fine. **Do not redistribute the
weights, and do not bundle them with anything.** They are cached outside the repo
(`%LOCALAPPDATA%\VocalTrainer\separator-models`) partly so they cannot be committed by
accident.

---

## Bottom line

There is no licensing blocker. The app ships under JUCE's free commercial Starter tier
with our own MPM implementation and our own 1-Euro filter — zero copyleft
in the binary. The single live copyleft edge is the ASIO SDK, which matters
only on distribution and is trivially avoided by leaving `VT_ENABLE_ASIO=OFF`. Every
GPL / AGPL / non-commercial component in this ecosystem lives in the offline Python tool,
which is a separate process that never links against the app.
