# Third-party code in NAM Player

Everything here is unmodified upstream source, copied at the commits below. `build.py`
(in the workspace root) compiles it; nothing is fetched at build time.

| Folder | What | Upstream | Commit / version | Licence |
|---|---|---|---|---|
| `NeuralAmpModelerCore/` | NAM's reference inference library (the `NAM/` folder). Runs A1 WaveNets, LSTMs, custom A2 shapes. | https://github.com/sdatkinson/NeuralAmpModelerCore | `d3189ed482ded880c128bf312941266c4fa5ceec` (v0.6.0 + fixes, 2026-11-10) | MIT, Copyright (c) 2023 Steven Atkinson (`NeuralAmpModelerCore/LICENSE`) |
| `tone3000-embedded/` | Generated C/NEON engines for the standard A2-Lite and A2-Full shapes (`engines/cortex-a76/`). | https://github.com/tone-3000/embedded | `b84b60c652cb386ea341be50b8c665ae8a489cff` (2.0.0) | MIT, Copyright (c) 2026 TONE3000 (`tone3000-embedded/LICENSE`, `NOTICE`) |
| `eigen/` | Linear algebra for NAM Core (`Eigen/` and `unsupported/Eigen/`). | https://gitlab.com/libeigen/eigen | `bc3b39870ecb690a623a3f49149a358b95c5781d` (5.0.1), the commit NAM Core pins | MPL-2.0, with some files under BSD / Apache-2.0 (`eigen/COPYING.*`) |
| `nlohmann/json.hpp` | JSON parser (.nam files are JSON). | https://github.com/nlohmann/json | 3.12.0, the copy NAM Core ships | MIT, Copyright (c) 2013-2025 Niels Lohmann |

The two profiles in `../Content/Models/` are NeuralAmpModelerCore's example models
(`example_models/A2.nam` and `example_models/wavenet_a1_standard.nam`), from the same
MIT-licensed repository.

To update a library: replace its folder with the new upstream copy, then run
`python3 build.py`, `python3 tests/run_tests.py` and `sh tests/run_linux_arm.sh`.
The regression tests compare the plugin's audio with NAM Core, so a behavioural change
in either engine shows up there.
