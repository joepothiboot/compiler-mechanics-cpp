# Compiler Optimization Techniques in C++

A guide to what an optimizing C++ compiler (primarily Clang/LLVM, with GCC equivalents noted) does to your code, why SSA form makes it possible, and how you can help or hinder it.

Every technique follows the same four-part structure:

1. **Mechanism**: how the IR and SSA form make the transformation legal and cheap.
2. **Before**: realistic C++ as you'd write it.
3. **After**: what the compiler turns it into (C++ pseudocode or LLVM IR), plus the hardware consequence.
4. **Controls**: flags, attributes and language features that enable, block or inspect it.

## Chapters

| # | File | Topics |
|---|------|--------|
| 1 | [01_ir_ssa_cleanups.md](01_ir_ssa_cleanups.md) | Constant folding/propagation, DCE, CSE, GVN, canonicalization |
| 2 | [02_control_flow.md](02_control_flow.md) | Jump threading, tail merging, SCCP |
| 3 | [03_loop_transformations.md](03_loop_transformations.md) | LICM, unrolling, unswitching, fusion/fission, interchange, tiling |
| 4 | [04_strength_reduction_vectorization.md](04_strength_reduction_vectorization.md) | Strength reduction, IV elimination, SIMD, loop-carried dependencies, early exits |
| 5 | [05_alias_analysis_memory.md](05_alias_analysis_memory.md) | TBAA, points-to, `__restrict__`, load forwarding, escape analysis |
| 6 | [06_bounds_check_elimination.md](06_bounds_check_elimination.md) | Bounds-check elimination via branch/range propagation |
| 7 | [07_interprocedural.md](07_interprocedural.md) | Inlining, function specialization, `pure`/`const`, LTO, PGO |
| 8 | [08_hardware_level.md](08_hardware_level.md) | FMA contraction, alignment, cache-line layout, false sharing |

Chapters 1–2 are the foundation: almost every later pass assumes the IR has already been cleaned up by them. The chapters build on the repo's study files. [ir_and_ssa/05_ssa_construction.cpp](../../ir_and_ssa/05_ssa_construction.cpp) builds the SSA form these passes consume, and [analysis_and_codegen/06_dataflow_analysis.cpp](../../analysis_and_codegen/06_dataflow_analysis.cpp) implements the lattice framework that SCCP and range analysis are instances of.

## How the claims were checked

Snippets marked **✓ verified** were compiled with Apple clang 21 (`-O2`, arm64), and the IR or assembly shown is what the compiler actually produced, trimmed for length. Unmarked "after" code is illustrative pseudocode: it shows the shape of the transformation, not literal compiler output. x86-64 notes describe typical codegen for those targets.

## Inspecting the optimizer yourself

```bash
# Optimized LLVM IR with readable value names
clang++ -std=c++17 -O2 -S -emit-llvm -fno-discard-value-names f.cpp -o f.ll

# Assembly
clang++ -std=c++17 -O2 -S f.cpp -o f.s

# Optimization remarks: what fired, what was missed, and why
clang++ -O2 -c f.cpp -Rpass=loop-vectorize -Rpass-missed=loop-vectorize -Rpass-analysis=loop-vectorize
clang++ -O2 -c f.cpp -Rpass='licm|gvn|inline|loop-unroll'
clang++ -O2 -c f.cpp -fsave-optimization-record     # YAML remarks for every pass

# Pipeline dump (very verbose): IR after each pass
clang++ -O2 -c f.cpp -mllvm -print-after-all 2>&1 | less

# GCC equivalents
g++ -O2 -fopt-info-vec-all -fopt-info-loop-all f.cpp
g++ -O2 -fdump-tree-all f.cpp          # GIMPLE after every tree pass
```

[Compiler Explorer](https://godbolt.org) is the fastest way to compare compilers and flags side by side, and its "Opt Pipeline" view shows IR diffs pass by pass.

On macOS, `g++` is Apple clang. Install real GCC (`brew install gcc`, then `g++-14`) to try the GCC flags.

## Optimization level cheat sheet

| Level | Clang | GCC |
|-------|-------|-----|
| `-O0` | No optimization. Every variable lives in memory (`alloca`), and `mem2reg` doesn't run, so there is no SSA promotion. | Same |
| `-O1` | Core scalar cleanups, without the aggressive inlining and vectorization that `-O2` adds | Core cleanups, no vectorization |
| `-O2` | Full scalar pipeline, inlining, **loop + SLP vectorization** | Since GCC 12: vectorization with the "very-cheap" cost model |
| `-O3` | More aggressive inlining/unrolling, `-O2` + extra loop passes | Full vectorizer cost model, `-floop-interchange`, `-funswitch-loops`, … |
| `-Os`/`-Oz` | Size-optimized; vectorization/unrolling restrained | Same |
| `-Ofast` | `-O3 -ffast-math` (deprecated in recent Clang; spell out the flags) | `-O3 -ffast-math` |
