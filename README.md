# Vocal Trainer

A vocal trainer for singers. It shows your sung pitch against the notes of a
song, in real time, so you can see *where* you are flat or sharp rather than just being told
that you are.

![Practising against a chart: target notes scrolling right to left, with the live sung pitch traced against them](docs/demo.gif)

Blue bars are the target notes and the bright one at the now-line is the note due right now.
The green trace is the live sung pitch, and the faint grey line behind it is the reference
contour the chart was built from. Bottom left reads out cents, clarity and input level.
*(The lyric strip is spliced out of this capture.)*

Two halves:

- **The app** (C++ / JUCE 9) — a **standalone** desktop application. It reads the mic,
  runs a hand-rolled MPM / NSDF pitch detector on a 16 kHz analysis stream, and draws a live
  pitch trace on a note highway. It owns the audio device itself; there is no plugin build
  and nothing here is ever loaded by a host.
- **The offline pipeline** (Python) — pre-analyses a song into a `chart.json` note chart:
  vocal separation, pitch tracking, note segmentation, lyric alignment, syllabification.

Design doc: [`docs/DESIGN.md`](docs/DESIGN.md). It holds the exact numbers — filter corners,
window sizes, thresholds, colours, geometry. Read the sections relevant to your files before
writing code.

**This project's own source is MIT** ([`LICENSE`](LICENSE)). It does **not** ship JUCE — you
clone that yourself and accept its licence directly, which matters more than it sounds like:
see [Licence](#licence) below and [`THIRD-PARTY.md`](THIRD-PARTY.md).

Contributing a dependency? Read [`LICENSING.md`](LICENSING.md) first. The short version: zero
copyleft in the shipped binary, and there is a hard do-not-link list.

---

## Milestone status

| | | Status |
|---|---|---|
| **M0** | Validate the premise before writing code | done |
| **M1** | JUCE app: mic in, live pitch out | done |
| **M2** | Offline pipeline v1 → `chart.json` | **working end to end** |
| **M3** | Note highway + full-song practice | **feature-complete** |
| **M3.5** | Stage 0 separation + **Add song...** from any audio file | **working end to end** |
| ~~M4~~ | ~~VST3 target + Ableton sync + calibration~~ | **dropped** — standalone only |
| M5 | Lyrics end to end | not started |
| **M6** | Scoring, post-take review, take recording | **built** |
| **M6+** | Generated exercises, chart editing, monitor level | **built** |

---

## Building (Windows 11, VS Build Tools 2022, no full IDE)

**Windows only, currently.** Nothing in the code is Windows-specific by design, but no other
platform has been built or tested — treat macOS and Linux as unknown rather than supported.

The machine this was developed on has no Visual Studio IDE and no standalone CMake on `PATH`,
so the commands below use the CMake bundled with the Build Tools. If you have `cmake` and a
compiler on `PATH` already, plain `cmake` works and you can skip the `vcvars64.bat` step.

### One-time

JUCE 9.0.0 must be present as a **plain directory** at `./JUCE` (it is *not* a git submodule,
and it is gitignored). From the repo root:

```powershell
git clone --depth 1 --branch 9.0.0 https://github.com/juce-framework/JUCE.git JUCE
```

**You must also have a juce.com account and have accepted the JUCE 9 EULA** to use the free
Starter tier. An unregistered clone is AGPLv3, and that applies to whatever you build from it
regardless of this project's MIT licence. See [`THIRD-PARTY.md`](THIRD-PARTY.md).

### Configure and build

Open a plain PowerShell prompt in the repo root, then:

```powershell
# 1. Bring the MSVC toolchain into the environment (this is why we shell out to cmd)
cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && powershell'

# 2. Configure (Release)
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" `
    -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# 3. Build everything
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" `
    --build build
```

With CMake already on `PATH`, that is just:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Build a single target instead of everything with `--target VocalTrainer_Standalone`
or `--target vt_tests`.

For a Debug build use `-DCMAKE_BUILD_TYPE=Debug` and a separate build directory
(`build-debug`) — Ninja is single-config, so reusing one directory for both forces a full
reconfigure each time.

### Running it

```
build\VocalTrainer_artefacts\Release\Standalone\Vocal Trainer.exe
```

On first launch, open **Options → Audio/MIDI Settings** and pick the input device and the
mic channel. The app owns the audio device outright, and the choice is remembered between
runs.

**The toolbar shows only what applies right now.** With a chart loaded it is one row —
`Unload | Exercises... | |< Play Levels | Rec Listen Review | ...` — and the free-sing controls
(**Target**, **Free Sing**) are *gone* rather than greyed out, because a permanently disabled
control is a permanent question about something that is not available at all. **Levels** opens
the monitor and guide sliders; they are the widest controls in the app and the thing you touch
once a session, which is a bad trade for a row of the highway. The **...** menu holds what you
do between takes: **Add song...**, **Edit chart**, **Edit theme...**. Below about 700 px of
window width the transport drops onto a second row rather than clipping.

### Running the tests

```
build\vt_tests_artefacts\Release\vt_tests.exe
build\vt_chart_tests_artefacts\Release\vt_chart_tests.exe
build\vt_render_tests_artefacts\Release\vt_render_tests.exe
build\vt_backing_tests_artefacts\Release\vt_backing_tests.exe
build\vt_score_tests_artefacts\Release\vt_score_tests.exe
```

`vt_tests` covers the DSP core (12 cases). `vt_chart_tests` covers chart parsing, the
time queries, octave folding, and chart editing round-tripped through the writer
(66 checks) — and it parses the REAL `songs/synth-demo/chart.json`, so a schema drift
between the Python writer and the C++ reader fails there rather than on stage. Build a chart
first or those checks skip. `vt_render_tests` renders components offscreen and counts pixels,
which is the only thing that catches "it computes correctly but you cannot see it".

`vt_score_tests` covers scoring and exercise generation (70 checks). Two of its cases guard
bugs that would be **invisible while using the app** and would quietly teach you to sing
worse:

- **vibrato is not punished.** Textbook vibrato (5.5 Hz, ±50 cents) is perfectly in tune and
  must score as such. If scoring ever reverts to the instantaneous pitch this fails — and the
  only symptom in the app would be a stingy score you would blame on your own singing.
- **the trailing median does not straddle a note boundary.** A median that is not cleared
  between notes reports a stepwise scale as flat *and* late on every note, which reads
  exactly like a latency bug and would be debugged in completely the wrong place.

`vt_backing_tests` covers the `BackingPlayer` lifecycle. **It is the one test that crashes
rather than fails when it catches its bug** — JUCE's resampler takes an integer divide by
zero if the audio thread pulls it after `releaseResources()`, and a hardware fault cannot be
turned into a failed assertion. Exit code `0xC0000094` from that runner means the guard in
`BackingPlayer::renderInto` has been lost. See DESIGN.md §11.12. It also checks the two
buffers an exercise generates, because *which deck* each sound lands on is a pedagogical
decision that fails silently: put the starting reference pitch on the guide deck and the app
still works, you just lose your starting note the first time you turn the guide off.

Python side:

```
cd python && ..\.venv\Scripts\python.exe -m pytest tests -q
```

Disable the C++ test targets entirely with `-DVT_BUILD_TESTS=OFF`.

### Adding a song from an audio file (M3.5)

Click **Add song...**, pick any audio file, and wait. The app runs the whole
offline pipeline for you — vocal/instrumental separation, pitch tracking, note
segmentation, lyric transcription — then loads the finished chart.

A progress panel covers the highway with the current stage and an overall
percentage; **Cancel** stops it cleanly at the next stage boundary. (the first ever run
also downloads a 610 MB separation model).

Separation is minutes of GPU work, so it runs out-of-process and cannot stall the
audio callback. Preparing songs is a before-you-play activity, which is why the
button lives in the **...** menu rather than on the transport row.

Video containers (`.mp4`, `.mkv`, …) work too; ffmpeg demuxes them first.

The equivalent from a prompt, which also takes the options the button does not:

```bash
vocalchart add-song "D:\music\Mr Brightside.mp3" --lyrics brightside.txt
```

**Pass `--lyrics` whenever you have the text.** It is the single largest quality
difference available in the whole pipeline: known lyrics turn a 20–38% word
error transcription problem into a pure timing problem. Without it the chart
still gets words, but they will be wrong often enough to notice.

### Practicing against a chart (M3)

1. Build a chart for a song — **Add song...**, `add-song`, or `import-stems`.
2. Launch the Standalone, click **Load chart...**, pick `songs/<id>/chart.json`.
   (A song added with **Add song...** loads itself when it finishes.)
3. Press **Play**. The backing track plays, the highway scrolls; sing.

Try it immediately with the bundled demo: `songs/synth-demo/chart.json`.

**Wear headphones.** If you practice on speakers, the mic hears the backing track and the
tracker follows the record instead of you. That is the single most likely cause of
"the pitch detection is broken" that is not actually a bug.

**The clock follows what you hear.** When a backing track is loaded, its playback position
*is* chart time. With no backing track the clock falls back to the audio-stream position —
estimates arrive at a steady 125 Hz even during silence, so the stream is drift-free and is
already the time base those estimates are stamped in. Either way the highway can never drift
against the audio.

The backing mix happens strictly *after* the analysis chain reads the mic, so the backing
track can never leak into the pitch detector.

Two things the highway does deliberately, which can look like bugs:

- **The target bar gets taller when you loosen the tolerance.** Its drawn height *is* the
  full-credit window (2 x `tFullCents`), so "inside the bar" means "full marks", with
  nothing to translate in your head.
- **Singing an octave off still puts the trace on the bar**, with a `-1 8ve` badge.
  Scoring is octave-agnostic, so the display folds the same way. If it did not, a bass
  singing a soprano line would see full marks with the trace nowhere near the bar, and
  would stop trusting the feedback.

Every geometry and colour constant lives in `%APPDATA%\VocalTrainer\theme.json` and
hot-reloads within 250 ms — no rebuild needed to tune the look.

**Load chart... opens in your song folder** with no path configured anywhere. It resolves,
in order: the folder you last loaded a chart from, then the repo's own `songs/` directory
found by walking up from the executable, then `%APPDATA%\VocalTrainer\songs`. That mirrors
`_default_library()` in the Python CLI, so the tool that *writes* a song and the app that
*reads* it agree on where songs live without either naming a path. It is remembered in
`%APPDATA%\VocalTrainer\VocalTrainer.settings` — never in serialised session state, because
an absolute path baked into a saved session breaks permanently the day the library moves.

### Warm-ups: scales, arpeggios and interval drills

Click **Exercises...** and pick one. Ten of them, from long tones to the full twelve-interval
drill, with a starting note, a tempo and a **length** in the same menu. They are *generated as
charts*, so the highway, the lyric strip (solfège syllables), the scoring and the review all
work on a warm-up exactly as they do on a song — and the numbers are comparable between the two.

**It starts by playing you your note.** A piano tone at the first pitch of the exercise rings
on its own, then four count-in clicks, then you sing — which is what a teacher does before
counting you in. That reference is on the *click* deck, not the guide deck, so turning **Guide**
off to test your own ear does not also take away the note you are supposed to start on.

The running guide — one struck piano note per note of the exercise — is on the guide deck, and
**Guide** and its level silence it. Silencing it is the point: a reference you cannot turn off
trains you to follow rather than to pitch. Notes longer than the tone's decay are re-struck
every three beats, as an accompanist would, so a long tone still has something to check against
in the last bar where it is hardest to hold steady.

**Length is a duration, not a repeat count** — Short (0:45), Medium (1:45) or Long (3:30),
remembered between sessions. One "repeat" means five seconds of a five-note run and the better
part of a minute of the interval drill, so a repeat count would make some exercises trivial and
others interminable; the tempo changes the answer too. Whichever length you pick, an exercise
never does less than one whole pass of itself.

Exercises that transpose walk **up to a turning point and back down** rather than climbing a
semitone per repetition forever, bounded by your range, an octave of climb, and half the
repetitions. Ninety seconds of five-note runs is twenty-odd repetitions, and climbing all of
them would leave the top of the exercise three octaves above where it started. Each repetition
is named after the key it is in (*"Run 9 (F4)"*), so the review's worst-phrase list tells you
where in your range the wheels come off.

The interval drill is the one worth doing regularly — the review groups your errors by the
interval you leapt into, so it ends with something as specific as *"weakest interval:
ascending 6th, −34 cents over 4 attempts"*.

### After the take: recolouring, review, and listening back

- **Notes recolour once they have been sung**, green through vermilion by accuracy, with the
  part you actually sang filled solid — so a note sung well but cut short reads differently
  from one sung badly, and differently again from one never sung (hollow grey). Notes are
  never recoloured *while* they are active: during the note the geometry (trace inside the
  bar) is the signal, and a bar changing colour under the dot would compete with it. Set
  `highway.showScoredNotes` to `false` in `theme.json` to switch it off.
- **Rec** records the mic to `songs/<id>/takes/take-<stamp>.wav` and starts the song. It
  records the same mono sum the detector analysed, before monitoring and the backing track,
  so the take is exactly the signal the score came from. **Listen** plays it back against the
  backing track; scoring is suspended while it does.
- **Review** appears automatically when the song ends. It leads with the one most actionable
  thing — a systematic *"you sat consistently 22 cents flat"* is a single habit to fix, where
  a percentage is a scoreboard — then a cents-error histogram, the weakest phrases worst-first
  (click one to loop it), and the interval breakdown. No score number, no grade, no streak:
  DESIGN.md §6 is explicit that the live view carries none of that, and moving it to the
  review screen would only relocate the gamification.

### Fixing a chart by hand

ASR sits around 30% word error on sung vocals and the note segmenter targets an F-measure of
0.70, so charts come out wrong sometimes. Tick **Edit chart** in the **...** menu, click a note
on the highway, and fix the three things automatic charting actually gets wrong:

- **a wrong word** — retype it, or retype the whole **Line** and press Return to re-flow it
  one word per note across the phrase. That is the fast path: the timing is already right,
  it is only the words that are wrong.
- **a note an octave out** — the dominant segmenter failure, so `-12` / `+12` get their own
  buttons.
- **a note slightly early, late or long** — 10 ms and 25 ms nudges, clamped so notes can
  never be made to overlap.

**Save chart** writes back to `chart.json`, keeping the previous version as `chart.json.bak`.
It re-reads the file and merges, so every field this build does not parse — `$schema`, the
`timing` block, `referenceAudioSha1`, the generator metadata, and the whole contour — survives
untouched. Correcting one word must not silently delete the metadata the offline pipeline
depends on. Editing lyrics also flips `lyricSource` to `manual`, which removes the
"auto-transcribed" badge, because once a human has checked the words leaving it up is its own
kind of lie.

## Build options

| Option | Default | Effect |
|---|---|---|
| `VT_ENABLE_ASIO` | **OFF** | When ON, sets `JUCE_ASIO=1` and compiles ASIO support into the app. **Do not distribute a binary built with this ON** — see below. |
| `VT_BUILD_TESTS` | ON | Builds the console test runners. |

### About `VT_ENABLE_ASIO`

Turning it **ON** gives the app direct low-latency access to the Apollo Twin X
through its ASIO driver, instead of going through WASAPI. That is a real, audible improvement
for live monitoring, and it is the reason the option exists. The ASIO SDK is bundled with
JUCE 9 (since 8.0.11), so nothing needs to be downloaded — just reconfigure with
`-DVT_ENABLE_ASIO=ON`.

It defaults to **OFF** for two reasons:

1. **The first build on a clean machine must not fail on an SDK problem.** Off by default
   means configure-and-build works immediately, every time.
2. **Licensing.** The ASIO SDK is **GPLv3 or Steinberg-proprietary**. For a personal,
   undistributed tool this never matters — no distribution, no copyleft obligation. But
   **for any closed-source distribution it must stay OFF** (JUCE falls back to WASAPI), or
   you must obtain Steinberg's proprietary ASIO licence. See `LICENSING.md`.

If you enable ASIO, note the startup order that this rig needs: Apollo powered and its
console running *before* the app.

---

## The offline pipeline

Python 3.11 venv at `.venv`, ONNX-only — `swift-f0` needs no `torch`, and **`torch` must not
be added as an M2 dependency**. `ffmpeg` must be on `PATH`.

```powershell
py -3.11 -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install -e python
```

### Commands

| | |
|---|---|
| `vocalchart add-song <audioFile>` | **everything**: one file in, a playable song folder out |
| `vocalchart import-stems <stemDir> <songDir>` | same, from a real multitrack — **better, when you have one** |
| `vocalchart separate <songDir>` | stage 0 only: `source.*` → `stems/vocals.wav` + `backing.wav` |
| `vocalchart f0 / notes / lyrics <songDir>` | stages 1 / 2 / 3 on their own |
| `vocalchart build <songDir>` | stages 0–5 over an existing song folder |
| `vocalchart inspect <songDir>` | human summary of what came out, with a pitch sparkline |

Every stage caches; `--force` recomputes, `-v` narrates.

### The separation venv (`.venv-separate`)

Stage 0 runs in a **second venv**, reached by subprocess. `audio-separator`
requires `torch >= 2.3`, and torch must not go anywhere near `.venv`: that one is
ONNX-only by design, and installing torch *after* `faster-whisper` is exactly
what produces the `cudnn_ops64_9.dll` failure DESIGN.md:375 warns about.

One-time setup:

```powershell
py -3.11 -m venv .venv-separate
.venv-separate\Scripts\python.exe -m pip install `
    torch==2.8.0 torchaudio==2.8.0 torchvision==0.23.0 `
    --index-url https://download.pytorch.org/whl/cu126
.venv-separate\Scripts\python.exe -m pip install `
    "audio-separator[gpu]==0.44.5"
```

Three things about that recipe are load-bearing:

- **`cu126`, not the `cu124` in DESIGN.md.** The cu124 index stopped carrying
  torch after 2.6; 2.8.0 is only on cu126 and cu128.
- **`torchvision` is pinned and installed alongside torch.** `audio-separator`
  depends on `onnx2torch`, which depends on `torchvision`. Leave it out and pip
  resolves torchvision to its newest release, which drags in a **CPU-only**
  torch over the CUDA one you just installed — silently, with only a dependency
  warning. Separation still works; it just takes 20–40 minutes instead of 2.
  Check with `python -c "import torch; print(torch.cuda.is_available())"`.
- **Re-run the torch install last if you ever add packages to this venv**, for
  the same reason.

No NVIDIA GPU? Use `/whl/cpu` and `audio-separator[cpu]`; everything works, just
slowly. Point `VOCALCHART_SEPARATOR_PYTHON` at a different interpreter to
override discovery, and `VOCALCHART_SEPARATOR_MODELS` to move the 610 MB model
cache out of `%LOCALAPPDATA%\VocalTrainer\separator-models`.

### Separation quality, measured here

Mixing a known vocal stem back over its own backing track and separating it
again recovers the vocal at **16.9 dB** scale-invariant SDR, and — the number
that actually matters, since this feeds a pitch chart — the pitch track off the
separated stem matches the ground-truth stem to **0.0 cents median, 99.4% of
frames within 50 cents**. Warm separation of a 30 s clip takes 16 s on the
RTX 3080 Ti.

That is good, and it still does not make separation the preferred path. A
**genuine isolated stem** drops downstream word error to 14.19% against 21.08%
for a separated one (DESIGN.md section 4) — the artifacts that survive
separation hurt the lyrics far more than they hurt the pitch. Use
`import-stems` when you have real multitracks; use `add-song` for everything
else.

---

## Repository layout

```
CMakeLists.txt        build configuration for the app + tests
docs/DESIGN.md        the full design document — the source of truth for all numbers
LICENSE               MIT — this project's own source
THIRD-PARTY.md        attribution for everything we depend on, and what you may redistribute
LICENSING.md          licence decisions and the do-not-link list (read before adding a dep)
songs/synth-demo/     the one bundled song — synthesised, no third-party rights
src/                  C++ app source (namespace vt)
  AudioEngine.cpp       audio thread, lock-free queues, monitor + backing mix
  MainComponent.cpp     the UI shell
  app/AppSettings.cpp   PropertiesFile: song library root, remembered folders, monitor level
  audio/BackingPlayer.cpp  three decks: backing, guide vocal, recorded take
  audio/TakeRecorder.cpp   mic -> WAV, lock-free from the audio thread
  chart/Chart.cpp       the parsed chart, plus the narrow hand-editing API
  chart/ChartWriter.cpp save an edited chart without losing unparsed fields
  dsp/AnalysisChain.cpp resample, condition, MPM pitch detection
  exercise/ExerciseLibrary.cpp  scales and interval drills, generated as charts
  exercise/ExerciseAudio.cpp    their metronome click and pitch guide tone
  score/TakeScorer.cpp  per-note scoring off a trailing median, and the take summary
  import/SongImporter.cpp  spawns the offline pipeline, parses its progress
  ui/Theme.cpp          Okabe-Ito colours, fonts, metrics
  ui/PitchLaneComponent.cpp   the pitch lane / live trace
  ui/NoteHighwayComponent.cpp the scrolling highway, incl. accuracy recolouring
  ui/ImportOverlayComponent.cpp  the "preparing your song" panel
  ui/ReviewOverlayComponent.cpp  the post-take review screen
  ui/ChartEditPanelComponent.cpp fix a word, a pitch or a timing by hand
tests/test_pitch.cpp  pitch detector tests (vt_tests console app)
tests/test_score.cpp  scoring + exercise generation (vt_score_tests)
JUCE/                 JUCE 9.0.0 clone — gitignored, not a submodule
.venv/                Python 3.11 venv for the offline pipeline — gitignored
.venv-separate/       Python 3.11 venv holding torch + audio-separator — gitignored
build/                CMake build tree — gitignored
```

---

## Licence

**MIT**, for everything in this repository. See [`LICENSE`](LICENSE).

Three caveats that MIT does not cover, in descending order of how likely they are to catch
you out. [`THIRD-PARTY.md`](THIRD-PARTY.md) has the full detail.

1. **JUCE is not ours to license to you.** It is gitignored, you clone it yourself, and JUCE 9
   is dual-licensed AGPLv3-or-commercial. This project is developed under JUCE's free
   **Starter** tier, which is perpetual and costs nothing but requires a juce.com account and
   acceptance of the EULA. **An unregistered clone is AGPLv3**, and so is anything you build
   from it — our MIT grant does not change that.

2. **Do not distribute a build made with `VT_ENABLE_ASIO=ON`.** The ASIO SDK is GPLv3 or
   Steinberg-proprietary. Off by default, and it stays off in anything released here.

3. **Charts you generate are derivative works of the songs they came from** — the aligned
   lyrics especially. `songs/` is gitignored for that reason. Keep them local. The one
   exception, `songs/synth-demo/`, is synthesised here and carries no third-party rights.

The separation model weights are also not redistributable. They download to
`%LOCALAPPDATA%\VocalTrainer\separator-models`, outside the repo, so they cannot be committed
by accident.
