# NAM Player

A Neural Amp Modeler profile player for Akai MPC standalone hardware, by Vazgen. It is an
insert effect: put a track through an amp, pedal or outboard capture (`.nam` file), and
browse the captures on the card with two knobs. Built for the **MPC Sample**, where it
runs as a Knob FX; it also runs on the MPCs with touch screens.

It plays NAM **A2** profiles (the 2026 architecture, "NAM v2") and the older **A1**
WaveNet and LSTM profiles.

## Where the profiles go

```
<card>/
├── VST2 Plugins/
│   └── Vazgen - VST - NAM Player/      the plugin (this folder)
└── NAM Profiles/                       your profiles: create this folder beside "VST2 Plugins"
    ├── Marshall/                       each folder with .nam files is one position of the Folder knob
    │   ├── Crunch.nam                  each file is one position of the Model knob
    │   └── Lead.nam
    └── Clean.nam                       files directly in "NAM Profiles" appear in a folder called "Profiles"
```

- Folders can be nested up to three levels; each one that holds `.nam` files becomes a folder on the knob. Folders and files are in alphabetical order.
- `NAM Profiles` is outside the plugin folder on purpose: updating the plugin never touches it.
- Two demo profiles ship inside the plugin and appear as the folder **Factory** (last in the list).
- Keep names short. The MPC Sample shows about nine characters per knob, and the Model knob puts the position first (`3 Crunch`).
- Keep folders to about 100 profiles: the knob has a limited number of steps.
- The list is read when the effect is loaded, and again whenever the live link (below) adds or removes a profile. After changing the card by hand, load the effect again.

## The live link: sending profiles while it plays

An MPC Sample that is powered on normally and connected with USB-C puts a small network link on the cable; it is how Akai's own web firmware updater at `mpc-sample.local` works, from a computer or an iPhone. While NAM Player is loaded as a Knob FX, it answers on that link too:

**http://mpc-sample.local:9626** (or http://192.168.155.1:9626)

The page is the plugin's screen on a device that gives it none. Three things never leave it: **what is playing** (with previous and next buttons), the **In, Out and CPU meters**, and the four **sound sliders** (Drive, Mix, Tone, Level). On a computer they sit in a column on the left; on a phone the playing bar stays at the top and the sliders fold away under "Sound".

Everything else is one tab at a time:

- **On the card**: the folders and profiles on the card, with a search. Tap a profile to play it. "Edit" shows the remove buttons, which ask once more before deleting;
- **TONE3000**: see below;
- **Community**: 259 captures on GitHub; "Send" saves one to the card (folder "Community" unless you change it) and plays it. The phone or computer downloads it, so that device needs its own internet connection;
- **Upload**: send `.nam` files from that computer or phone, by choosing them or by dropping them anywhere on the page. They are saved to `NAM Profiles` on the MPC's card and the last one starts playing at once. No SD Card Access mode, nothing to eject, the audio keeps running.

The button at the top of the playing bar opens the **settings**: Quality, Normalize, and what is running right now (engine, levels with peaks, CPU load with its peaks and a count of late blocks, the MPC's sample rate and block size). The controls move the real parameters.

A profile chosen from the page stays selected until you turn the Model or Folder knob.

**TONE3000 is in the page too.** Paste your publishable key once (tone3000.com → Settings → API Keys; it is saved on the card). "Browse TONE3000" opens TONE3000 in a new tab, where you sign in, audition and pick a tone; TONE3000 sends the result back to the plugin's address, the page collects it, lists the tone's models, and "Send" puts one on the card and plays it. The browser does the signing in and downloading; the plugin only catches the answer and stores the key. If TONE3000 refuses the return address, register the address the page shows as a redirect URI on your key. **This flow is written from TONE3000's published API and has not been run against the live service yet.** The companion on a computer (`python3 companion/nam_card_loader.py`) remains as a second route.

What the link does and does not do:

- It listens only on USB network interfaces, never on Wi-Fi or Ethernet, so on an MPC that has those it is not reachable from the wider network. Whoever is at the other end of the USB cable can use it; there is no password.
- It writes `.nam` files and `ABOUT.txt` notes inside `NAM Profiles`, and nothing else. A file that is not a NAM model is refused.
- The plugin never connects out to anything. It has no way to reach the internet; the browser at the other end does the downloading.
- It exists only while NAM Player is loaded, and only the first instance has it.
- In SD Card Access mode the MPC switches its USB port to card-reader duty and the link is gone until you leave that mode.

**Works on an MPC Sample** (firmware 1.4.0.8, 25 November 2026): the page opens, profiles sent from a Mac were saved and played, and the controls and meters report from the device.

### Checking the plugin on the device

`POST /api/capture?seconds=4` makes the plugin record what it receives, what goes into the model, what the model puts out and what the plugin puts out; `GET /api/capture.wav` returns it as a 4-channel WAV. `tests/device/check_capture.py` does both and compares the model's output on the MPC with NeuralAmpModelerCore run on the same input on your computer. On the MPC Sample above, with a TONE3000 A2 profile and a test file played from a pad, the two were 95 dB apart: the same audio.

## Controls

| # | Parameter | Range | What it does |
|---|---|---|---|
| 0 | Level | -24 to +12 dB | Output level |
| 1 | Drive | -24 to +24 dB | Gain into the model: how hard the amp is pushed |
| 2 | Mix | 0 to 100 % | Dry / wet. The dry signal stays stereo and is delayed to match the wet one, so blends stay in phase |
| 3 | Tone | 500 Hz to 20 kHz, Off | Low-pass on the wet signal. Amp-head captures have no speaker in them and sound fizzy; turn this down to stand in for a cabinet. Fully up is Off |
| 4 | Quality | Auto / Lite / Full | Which version of an A2 profile runs (A2 files hold a light and a full model). Auto runs Full and drops to Lite if the CPU cannot keep up. No effect on profiles that have only one size |
| 5 | Normalize | Off / On | Brings every profile to the same loudness (-18 dB), using the loudness stored in the file. Files without it are left alone |
| 6 | Model | list | The profile, within the current folder |
| 7 | Folder | list | The folder |

**MPC Sample knobs:** Model, Drive, Mix. With SHIFT: Folder, Tone, Level. Quality stays on Auto and Normalize stays on.

**Touch-screen MPCs:** one page, AMP. Top row: Model, Drive, Tone, Mix. Bottom row: Folder, Quality, Normalize, Level.

Four built-in presets (Default, Pushed, Dark Crunch, Parallel) set the sound controls and leave the profile selection alone.

On the Model knob, `ERR` in front of a name means the file could not be loaded (the audio passes through unprocessed), and `CPU!` means the model was too heavy and has been bypassed (see below). The text is whatever the MPC last asked for, so it may only update when the knob next moves.

A project remembers its profile by folder and file name, not by knob position, so adding profiles later does not change what an old project loads. If the file is gone, the knob positions decide.

## What it needs

- **MPC Sample:** firmware v1.4.0 or later. In MIDI Config, in the Dev Enhancements (Beta) section, turn on Dev Enhancements (Beta) and then Custom Plug-In Support. The effect appears at the bottom of the FX Select list.
- **Other MPCs:** MPC OS 3.10.1 or later. Preferences → Enhancements (Beta) → Enable Enhancements and External Plugin Support. The effect appears in the browser under External Plugins.

## CPU

A neural amp model is far heavier than an ordinary effect. **Measured on an MPC Sample** (firmware 1.4.0.8, 128-frame blocks): an A2 profile at full size takes **39 % of one core** (peaks 43 %, no late blocks), and pass-through with the sample-rate converters takes 2 %. That is two to three times the estimate below for A2-Full, so on this device treat the right-hand column as optimistic and use Quality: Lite when you need headroom.

| Profile type | One M1 Pro core (measured, whole plugin) | MPC Sample, estimated share of one core |
|---|---|---|
| A2-Lite | 0.6 % | roughly 3 to 5 % |
| A2-Full | 2.0 % | roughly 10 to 20 % |
| A1 standard | 4.5 % | roughly 25 to 40 % |
| A1 feather / nano | not measured | well below A1 standard |

The right-hand column is an estimate, scaled from TONE3000's published Cortex-A76 figures for the A2 engines and the assumption that the MPC Sample's reported Cortex-A55 needs two to three times the cycles. Treat it as a guide to what to try first: **prefer A2 profiles**, and on A1 prefer the lite, feather and nano sizes.

The plugin protects the MPC from a model that is too heavy:

- It times its own processing. If a model keeps using more than 90 % of real time, it is bypassed (the audio passes through) and the Model knob shows `CPU!`. Choose another profile to clear it.
- With Quality on Auto, an A2 profile that averages more than 50 % is replaced by its Lite version before that happens.

On the 32-bit MPCs (One, Live, X, Key, Force) expect only A2-Lite and the small A1 sizes to be usable.

## How it works

- **Two engines.** Standard A2-Lite and A2-Full models run on TONE3000's generated C/NEON engines, about 2.7 times faster than the reference library (measured on an Apple M1, which is ARM64 like the MPC Sample). Everything else (A1 WaveNets, LSTMs, custom A2 shapes) runs on Steven Atkinson's NeuralAmpModelerCore. A file takes the fast path only if its configuration matches the standard A2 shape exactly.
- **Sample rate.** The MPC runs at 44.1 kHz and NAM models are trained at 48 kHz; played at the wrong rate a model is detuned. The plugin converts to the model's rate and back with a 64-tap polyphase filter (100 dB rejection, flat to about 19 kHz). This adds 64 samples (1.5 ms) of latency, reported to the host.
- **Live link.** A second low-priority thread runs a deliberately small HTTP server (one connection at a time, 16 MB per file, 3 s timeouts). After a change it rescans the card into a fresh list and swaps it in, keeping the playing profile selected.
- **Loading.** A worker thread loads the selected file after the knob has rested for 120 ms; the audio thread fades the old model out, swaps, and fades the new one in (about 14 ms of silence in the wet signal). Nothing is loaded or allocated on the audio thread.
- **Mono model, stereo dry.** The model is fed the sum of left and right; the wet signal is the same on both sides.

## Tested

- The SDK host simulator passes with no failures and no warnings, natively and with the real ARM64 Linux library in a container.
- `tests/run_tests.py` (33 checks): the audio matches NeuralAmpModelerCore to better than -90 dB for A2-Full, A2-Lite, A1 standard, an LSTM and slimmable models; latency is exactly 64 samples; random block sizes give the same audio; switching is click-free; state recall survives a changed card; the CPU guard behaves as described.
- `tests/test_link.py` (25 checks): with audio running in real time, a profile sent over the link is saved, selected and heard; the companion finds the plugin and sends through it; requests from other web sites, path tricks, non-model files, garbage and half-finished uploads are refused without harm.
- `tests/run_linux_arm.sh`: real-time background loading and switching, the live link, and 1,100 load / unload cycles of the ARM64 library with a clean exit.

## Not tested, not implemented

- Run on one MPC Sample (firmware 1.4.0.8) only. Not checked there: A2-Lite and A1 CPU load, how the screen shows long value text, whether the knob reaches every profile in a long list, and saving and reloading a project. Not run on any other MPC.
- TONE3000 sign-in from the plugin's page is untested against the live service.
- The 32-bit ARM library builds and passes the validator but has not been executed (no 32-bit ARM runtime on the build machine).
- No stereo (dual-mono) processing, no noise gate, no impulse-response cabinet loader: use Tone, or full-rig captures that include the cabinet.
- No input calibration: Drive is set by ear.
- `.namb` (binary NAM) files are not read, only `.nam`.
- The profile list is not rescanned when the card is changed by hand while the effect is loaded (only when the live link changes it).

## Building

From the workspace root (the folder that holds `build.py` and the SDK clone in `sdk/`):

```
python3 build.py                 # native + arm32 + arm64 -> plugins/NAM Player/build/Vazgen - VST - NAM Player/
python3 tests/run_tests.py       # needs numpy and scipy
sh tests/run_linux_arm.sh        # needs Docker on an ARM64 computer
```

Use `build.py`, not `sdk/tools/build.py` directly: the third-party engines need C++20, extra include paths and a C compiler pass, which the wrapper adds without changing the SDK.

Environment variables for testing only: `NAM_PLAYER_SYNC=1` loads profiles inside `process()` so that offline renders hear the model; `NAM_PLAYER_TEST_LOAD=0.7` pretends every full-size model uses that share of real time; `NAM_PLAYER_PROFILES=<folder>` points the plugin at a profiles folder when it is not installed on a card; `NAM_PLAYER_LINK_BIND=<IPv4 address>` (or `off`) and `NAM_PLAYER_LINK_PORT=<n>` make the live link listen somewhere other than the USB interfaces.

### A note for other plugins built with this SDK

Plugins built with the SDK's default zig toolchain do not run their static destructors when the MPC unloads them, because the start-up file that normally does this comes with GCC. A plugin that has any (this one does; so does any plugin that calls `mpc::pluginFolder()` or `mpc::contentPath()`) leaves exit handlers pointing into unloaded memory, which crashes the host process when it exits. `src/NAMPlayer.cpp` works around it with a small `__attribute__((destructor))` function that calls `__cxa_finalize`.

## Licences

The plugin's own code is MIT (`LICENSE` in the repository root; the built plugin carries all notices in `Content/Licences.txt`). It contains NeuralAmpModelerCore (MIT, Steven Atkinson), TONE3000's A2 engines (MIT), Eigen (MPL-2.0) and nlohmann/json (MIT); see `third_party/README.md`. The two demo profiles are NeuralAmpModelerCore's example models. Profiles you download have their own licences: the companion tool writes each source's creator and licence into an `ABOUT.txt` beside the files.

Neural Amp Modeler is Steven Atkinson's project; this plugin is not affiliated with it, with TONE3000 or with Akai.
