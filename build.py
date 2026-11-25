#!/usr/bin/env python3
"""Build NAM Player with the MPC Plugin SDK.

    python3 build.py                 # native + arm32 + arm64 + output folder
    python3 build.py --target native # just the test build for this computer
    python3 build.py --verbose       # any other option of sdk/tools/build.py works too

Why a wrapper instead of calling sdk/tools/build.py directly: NAM Player links
third-party inference engines (NeuralAmpModelerCore needs C++20 and Eigen, the
TONE3000 A2 engines are C). The SDK's build script compiles src/*.cpp as C++17
with no extra include paths, so this script loads it as a module and, without
modifying the SDK:

  * switches the language level to C++20 and the optimisation level to -O3,
  * adds the third_party include paths and the defines the engines need,
  * compiles the third-party sources to object files once per target
    (build/obj/<target>/, reused until a source or a flag changes) and links
    them into the plugin.

Everything else (host_sim parameter dump, skins, version.xml, validation) is
the SDK's own code, unchanged.

The SDK is expected in ./sdk (a clone of inmusic-software/mpc-plugin-builder-sdk);
set MPC_SDK to use another copy.
"""

import concurrent.futures
import hashlib
import importlib.util
import os
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
SDK = os.environ.get("MPC_SDK", os.path.join(ROOT, "sdk"))
PROJECT = os.path.join(ROOT, "plugins", "NAM Player")
THIRD = os.path.join(PROJECT, "third_party")

NAM_CORE = os.path.join(THIRD, "NeuralAmpModelerCore")
T3K = os.path.join(THIRD, "tone3000-embedded")

# Largest block handed to an engine in one call. src/NamEngine.h uses the same number.
ENGINE_MAX_BLOCK = 96
# Usual block: 64 host frames at 44.1 kHz are 69-70 frames at a model's 48 kHz.
ENGINE_BLOCK_HINT = 70

INCLUDES = [
    os.path.join(THIRD, "eigen"),
    os.path.join(THIRD, "nlohmann"),
    NAM_CORE,
    os.path.join(NAM_CORE, "NAM"),
    os.path.join(T3K, "a2lite"),
    os.path.join(T3K, "a2full"),
]

DEFINES = [
    "NDEBUG",                      # Eigen's bounds checks cost several times the CPU
    "NAM_SAMPLE_FLOAT",            # NAM Core processes float, not double
    "NAM_ENABLE_A2_FAST",          # NAM Core's own A2 fast path (used for non-standard A2 shapes)
    "NAM_A2LITE_MAX_BUFFER_SIZE=%d" % ENGINE_MAX_BLOCK,
    "NAM_A2FULL_MAX_BUFFER_SIZE=%d" % ENGINE_MAX_BLOCK,
    "NAM_PLAYER_ENGINE_MAX_BLOCK=%d" % ENGINE_MAX_BLOCK,
]

C_ONLY_FLAGS = [
    "-O3", "-ffast-math", "-fPIC", "-fvisibility=hidden", "-ffunction-sections", "-fdata-sections", "-w",
    "-DNAM_A2LITE_BLOCK_HINT=%d" % ENGINE_BLOCK_HINT,
    "-DNAM_A2FULL_BLOCK_HINT=%d" % ENGINE_BLOCK_HINT,
]


def third_party_sources():
    cpp, c = [], []
    for sub in ("NAM", os.path.join("NAM", "wavenet")):
        d = os.path.join(NAM_CORE, sub)
        cpp += sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith(".cpp"))
    c.append(os.path.join(T3K, "a2lite", "nam_a2lite.c"))
    c.append(os.path.join(T3K, "a2full", "nam_a2full.c"))
    return cpp, c


def load_sdk_build():
    path = os.path.join(SDK, "tools", "build.py")
    if not os.path.isfile(path):
        sys.exit("MPC Plugin SDK not found at %s\n"
                 "  git clone https://github.com/inmusic-software/mpc-plugin-builder-sdk.git sdk\n"
                 "  (or set MPC_SDK to an existing clone)" % SDK)
    spec = importlib.util.spec_from_file_location("mpc_sdk_build", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def patch(B):
    B.COMMON_FLAGS[:] = ["-std=c++20" if f == "-std=c++17" else f for f in B.COMMON_FLAGS]

    orig_common_flags = B.Builder.common_flags

    def common_flags(self):
        flags = ["-O3" if f == "-O2" else f for f in orig_common_flags(self)]
        for inc in INCLUDES:
            flags += ["-isystem", inc]
        flags += ["-D" + d for d in DEFINES]
        return flags

    B.Builder.common_flags = common_flags

    def compile_objects(self, target):
        """Compile the third-party sources for one target; returns the object files."""
        if self.toolchain == "docker":
            raise B.BuildError("NAM Player's build wrapper supports the zig (default) and gcc toolchains, not docker.")
        cpp_src, c_src = third_party_sources()
        cxx_flags = [f for f in self.common_flags() if f != "-shared" and not f.startswith("-Wl,")] + ["-w"]
        c_flags = list(C_ONLY_FLAGS) + ["-D" + d for d in DEFINES] + (["-g"] if self.args.debug else [])
        for inc in INCLUDES:
            c_flags += ["-isystem", inc]

        if target == "native" or target == "desktop":
            if self.toolchain == "zig":
                cxx, cc, tflags = self.zig + ["c++"], self.zig + ["cc"], B.native_target_flags_for_zig()
            else:
                if not self.system_cxx:
                    return []
                cxx, cc, tflags = self.system_cxx, [B.shutil.which("cc") or "cc"], []
        else:
            zig_target, zig_cpu, gcc, gcc_arch, _, _ = B.ARM_TARGETS[target]
            if self.toolchain == "zig":
                cxx, cc = self.zig + ["c++"], self.zig + ["cc"]
                tflags = ["-target", zig_target, "-mcpu=" + zig_cpu]
            else:
                cxx, cc, tflags = [gcc], [gcc.replace("g++", "gcc")], list(gcc_arch)

        obj_dir = os.path.join(self.project, "build", "obj", target)
        os.makedirs(obj_dir, exist_ok=True)
        jobs = []
        for src in cpp_src + c_src:
            is_c = src.endswith(".c")
            argv = (cc + c_flags if is_c else cxx + cxx_flags) + tflags
            rel = os.path.relpath(src, THIRD).replace(os.sep, "_")
            obj = os.path.join(obj_dir, os.path.splitext(rel)[0] + ".o")
            stamp = obj + ".flags"
            digest = hashlib.sha1(" ".join(argv).encode("utf-8")).hexdigest()
            fresh = (os.path.isfile(obj) and os.path.isfile(stamp)
                     and os.path.getmtime(obj) >= os.path.getmtime(src)
                     and open(stamp).read() == digest)
            if not fresh:
                jobs.append((src, obj, stamp, digest, argv + ["-c", src, "-o", obj]))

        cached = len(cpp_src) + len(c_src) - len(jobs)
        B.log("  [%s] third-party engines: %d to compile, %d up to date" % (target, len(jobs), cached))

        def run_job(job):
            src, obj, stamp, digest, argv = job
            code, out = B.run(argv)
            if code == 0:
                with open(stamp, "w") as fh:
                    fh.write(digest)
            return src, code, out, argv

        if jobs:
            with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
                for src, code, out, argv in pool.map(run_job, jobs):
                    if code != 0:
                        B.log(out.rstrip())
                        raise B.BuildError("compiling %s for %s failed.\n  Command: %s"
                                           % (os.path.relpath(src, PROJECT), target, B.quote_cmd(argv)))

        all_objs = []
        for src in cpp_src + c_src:
            rel = os.path.relpath(src, THIRD).replace(os.sep, "_")
            all_objs.append(os.path.join(obj_dir, os.path.splitext(rel)[0] + ".o"))
        return all_objs

    def with_objects(method, target_of):
        def wrapped(self, *a, **kw):
            objs = compile_objects(self, target_of(*a, **kw))
            saved = self.sources
            self.sources = saved + objs
            try:
                return method(self, *a, **kw)
            finally:
                self.sources = saved
        return wrapped

    B.Builder.build_native = with_objects(B.Builder.build_native, lambda output: "native")
    B.Builder.build_arm = with_objects(B.Builder.build_arm, lambda target, output: target)


def main():
    B = load_sdk_build()
    patch(B)
    argv = sys.argv[1:]
    if "--target" in argv and argv[argv.index("--target") + 1] == "desktop":
        sys.exit("The desktop VST2 target is not wired up in this wrapper; use --target native and host_sim.")
    # The SDK dumps the parameter list from the native build and bakes it into the
    # touch UI. Folder / Model must look the same on every machine, so the plugin
    # skips its profile scan for that step.
    os.environ["NAM_PLAYER_NO_SCAN"] = "1"
    return B.main([PROJECT] + argv)


if __name__ == "__main__":
    sys.exit(main())
