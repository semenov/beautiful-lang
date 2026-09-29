#!/usr/bin/env python3
"""Build every benchmark, run each one several times, and print a Markdown table
of median wall time and median peak memory. Outputs are compared across
implementations, so every column provably does the same work.

Usage: python3 run.py [bench ...]      (RUNS=5 python3 run.py to change the run count)
Needs macOS (/usr/bin/time -l), clang, go, cargo, node, deno and bun.
"""
import os
import re
import shutil
import statistics
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(ROOT, "bin")
BENCHES = ["records", "trees", "sort", "words", "json", "maps", "csv", "nbody", "lines", "channels", "spawn"]
# ONLY=Go,Plumb python3 run.py   to compare just those
RUNTIMES = [r for r in ["Node", "Deno", "Bun", "Go", "Rust", "C", "Plumb"] if not os.environ.get("ONLY") or r in os.environ["ONLY"].split(",")]
RUNS = int(os.environ.get("RUNS", "3"))
BUN = shutil.which("bun") or os.path.expanduser("~/.bun/bin/bun")


def build(selected):
    os.makedirs(BIN, exist_ok=True)
    for bench in selected:
        source = os.path.join(ROOT, "c", f"{bench}.c")
        if os.path.exists(source):
            subprocess.run(["clang", "-O2", "-o", os.path.join(BIN, f"c_{bench}"), source], check=True)
        subprocess.run(["go", "build", "-o", os.path.join(BIN, f"go_{bench}"), f"./{bench}"],
                       cwd=os.path.join(ROOT, "go"), check=True)
    if "Rust" in RUNTIMES:
        subprocess.run(["cargo", "build", "--release", "--quiet"], cwd=os.path.join(ROOT, "rust"), check=True)
    compiler = os.path.join(ROOT, "..", "compiler")
    subprocess.run(["cargo", "build", "--release", "--quiet"], cwd=compiler, check=True)
    plumb = os.path.join(compiler, "target", "release", "plumb")
    for bench in selected:
        source = os.path.join(ROOT, "plumb", f"{bench}.plumb")
        if os.path.exists(source):
            subprocess.run([plumb, "build", source, "-o", os.path.join(BIN, f"plumb_{bench}")], check=True)


def commands(bench):
    script = os.path.join(ROOT, "js", f"{bench}.js")
    found = {}
    if os.path.exists(script):
        found.update({"Node": ["node", script], "Deno": ["deno", "run", "-q", script], "Bun": [BUN, script]})
    found["Go"] = [os.path.join(BIN, f"go_{bench}")]
    rust = os.path.join(ROOT, "rust", "target", "release", bench)
    if os.path.exists(rust):
        found["Rust"] = [rust]
    plumb = os.path.join(BIN, f"plumb_{bench}")
    if os.path.exists(os.path.join(ROOT, "plumb", f"{bench}.plumb")):
        found["Plumb"] = [plumb]
    native_c = os.path.join(BIN, f"c_{bench}")
    if os.path.exists(native_c):
        found["C"] = [native_c]
    return found


def measure(command):
    done = subprocess.run(["/usr/bin/time", "-l"] + command, capture_output=True, text=True)
    if done.returncode != 0:
        raise RuntimeError(f"{' '.join(command)} failed:\n{done.stderr}")
    seconds = float(re.search(r"([\d.]+) real", done.stderr).group(1))
    peak = int(re.search(r"(\d+)\s+maximum resident set size", done.stderr).group(1))
    return seconds, peak / 1048576, done.stdout


def main():
    selected = sys.argv[1:] or BENCHES
    build(selected)
    results = {}
    for bench in selected:
        reference = None
        for runtime, command in commands(bench).items():
            if runtime not in RUNTIMES:
                continue
            times, peaks = [], []
            for _ in range(RUNS):
                seconds, peak, output = measure(command)
                reference = reference or output
                if output != reference:
                    raise RuntimeError(f"{bench}/{runtime} printed a different result:\n{output}")
                times.append(seconds)
                peaks.append(peak)
            results[(bench, runtime)] = (statistics.median(times), statistics.median(peaks))
            print(f"{bench:8} {runtime:5} {results[(bench, runtime)][0]:6.2f} s "
                  f"{results[(bench, runtime)][1]:7.1f} MB", file=sys.stderr)

    print(f"\nMedian of {RUNS} runs · wall time / peak memory\n")
    print("| Benchmark | " + " | ".join(RUNTIMES) + " |")
    print("|---|" + "---|" * len(RUNTIMES))
    for bench in selected:
        cells = []
        for runtime in RUNTIMES:
            if (bench, runtime) in results:
                seconds, peak = results[(bench, runtime)]
                cells.append(f"{seconds:.2f} s · {peak:.0f} MB")
            else:
                cells.append("—")
        print(f"| {bench} | " + " | ".join(cells) + " |")


if __name__ == "__main__":
    main()
