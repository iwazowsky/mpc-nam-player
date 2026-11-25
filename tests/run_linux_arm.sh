#!/bin/sh
# Runs the ARM builds - the files an MPC actually loads - under Linux in a container.
#
#   python3 build.py && sh tests/run_linux_arm.sh
#
# Needs Docker (or colima / Docker Desktop) on an ARM64 computer such as an Apple
# Silicon Mac. It lays out a pretend SD card (VST2 Plugins/ + NAM Profiles/), then
# runs the SDK's host simulator and tests/lifecycle.cpp against the arm64 library:
# real-time loading on the worker thread, profile switching, the live link's web server,
# and 1100 load / unload cycles followed by a clean process exit. The 32-bit library is run the same way
# when the container runtime can execute 32-bit ARM code.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${MPC_SDK:-$ROOT/sdk}"
ZIG="$SDK/.sdk-tools/zigenv/bin/python -m ziglang c++"
OUT="$ROOT/plugins/NAM Player/build/Vazgen - VST - NAM Player"
WORK="${NAM_PLAYER_ARM_WORK:-$HOME/.cache/namplayer-armtest}"   # must be visible to the container runtime

rm -rf "$WORK"
mkdir -p "$WORK/card/VST2 Plugins" "$WORK/card/NAM Profiles/Amps"
cp -R "$OUT" "$WORK/card/VST2 Plugins/"
cp "$ROOT/plugins/NAM Player/Content/Models/"*.nam "$WORK/card/NAM Profiles/Amps/"

for spec in aarch64-linux-gnu.2.31:64 arm-linux-gnueabihf.2.31:32; do
  target="${spec%%:*}"; bits="${spec##*:}"
  $ZIG -std=c++17 -O2 -target "$target" -I "$SDK/framework" -I "$SDK/tools/host-sim" "$SDK/tools/host-sim/host_sim.cpp" -o "$WORK/host_sim$bits" -ldl -lpthread
  $ZIG -std=c++17 -O2 -target "$target" -I "$SDK/framework" "$ROOT/tests/lifecycle.cpp" -o "$WORK/lifecycle$bits" -ldl -lpthread
done

run() {  # platform, bits
  docker run --rm --platform "$1" -v "$WORK":/w debian:bookworm-slim sh -c '
    cd /w; SO="/w/card/VST2 Plugins/Vazgen - VST - NAM Player/VST/nam-player-arm'"$2"'.so"
    echo "--- host_sim, as on the MPC (worker thread)"
    ./host_sim'"$2"' "$SO" test --seconds 2 | grep -E "FAIL|WARN|CRASH|Result" ; echo "    exit code $?"
    echo "--- host_sim, test mode (the model runs inside the offline render)"
    NAM_PLAYER_SYNC=1 ./host_sim'"$2"' "$SO" test --seconds 2 | grep -E "FAIL|WARN|CRASH|Result|timing:" ; echo "    exit code $?"
    echo "--- real time, then load / unload cycles"
    ./lifecycle'"$2"' "$SO" '"$3"' "/w/card/NAM Profiles/Amps/Demo Classic A1.nam"; echo "    exit code $?"'
}

echo "=== arm64 (MPC Sample, MPC Live III, MPC XL) ==="
run linux/arm64 64 1100
echo "=== arm32 (MPC One, Live, X, Key, Force) ==="
run linux/arm/v7 32 200 || echo "(this container runtime cannot run 32-bit ARM code: the arm32 library is built and validated, but was not run)"
rm -rf "$WORK"
