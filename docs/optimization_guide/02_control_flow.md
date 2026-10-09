# 2. Control Flow Optimization

These passes reshape the **control-flow graph (CFG)**: they remove branches, merge blocks and prove whole paths unreachable. In SSA form, the CFG and the data are linked through φ nodes. A φ at a merge point is exactly the information "which value arrives along which edge", so these passes can reason about values *per edge* instead of per variable.

---

## 2.1 Jump Threading

### Mechanism
- Consider a block `B` that branches on a condition whose value is **known along some incoming edge**. Jump threading duplicates `B` for that edge and redirects the edge straight to the known successor. The branch is gone on that path.
- SSA makes the "known along this edge" question cheap. If the condition is (or depends on) a φ, look at the φ's incoming value for that predecessor: `%f = phi [1, %then], [0, %else]` and `br i1 %f` means "from `%then`, the branch always goes true."
- LLVM's `JumpThreading` pass also uses **LazyValueInfo**, which derives facts like "`x > 10` holds on this edge" from dominating comparisons.
- The cost is code duplication, so the pass has a size threshold (`-mllvm -jump-threading-threshold`).

### Before
```cpp
int classify(int x) {
    int flag;
    if (x > 10) flag = 1;
    else        flag = 0;

    if (flag) return x * 2;   // re-tests what the first branch already decided
    return x + 3;
}
```

### After: conceptual (jump threading)
```cpp
int classify(int x) {
    if (x > 10) return x * 2;   // threaded: edge from "then" jumps straight to "return x*2"
    return x + 3;               // edge from "else" jumps straight to "return x+3"
}
```

### After — ✓ verified (`-O2`, final IR)
```llvm
  %cmp = icmp slt i32 %x, 11
  %mul = shl nuw nsw i32 %x, 1
  %add = add nsw i32 %x, 3
  %retval.0 = select i1 %cmp, i32 %add, i32 %mul
  ret i32 %retval.0
```
After threading, `SimplifyCFG` goes one step further. Both arms are cheap, so it **if-converts** the remaining diamond into a branch-free `select` (AArch64 `csel`, x86 `cmov`).

**Hardware impact:** every removed conditional branch is one fewer entry competing for the branch predictor and one fewer chance of a misprediction, which costs about 15–20 cycles of pipeline flush on modern out-of-order cores. Correlated branches like the one above are a classic source of avoidable mispredictions when they sit far apart in a large function.

### Controls
| Control | Effect |
|---|---|
| `[[likely]]` / `[[unlikely]]` (C++20), `__builtin_expect` | Bias block layout and threading-cost decisions |
| `-fno-thread-jumps` (GCC) | Disables GCC's jump threading |
| `-mllvm -jump-threading-threshold=N` | Duplication size budget in LLVM |
| `__builtin_unpredictable(cond)` (Clang) | Hints the branch is data-dependent and random, favoring `select` over a branch |

---

## 2.2 Tail Merging (Cross-Jumping) and Code Hoisting/Sinking

### Mechanism
- **Tail merging** is the reverse of duplication. If two blocks *end* with identical instruction sequences and jump to the same successor, the common tail is emitted once and both blocks jump to it.
- At the IR level, LLVM's `SimplifyCFG` performs **sinking** (identical instructions at the end of predecessors move into the common successor, and their differing operands become φs) and **hoisting** (identical instructions at the start of both successors of a branch move up into the branch block).
- At the machine level, `BranchFolding` (LLVM) and `-fcrossjumping` (GCC) merge identical machine-instruction tails after register allocation.
- In SSA, sinking is mechanical. Two instructions `store %a, %p` and `store %b, %p` in the two predecessors become `%m = phi [%a, ...], [%b, ...]` plus a single `store %m, %p` in the successor.

### Before
```cpp
int record(int x, int* out) {
    if (x > 0) {
        *out = x;
        return 1;
    } else {
        *out = x;     // same tail in both arms
        return 1;
    }
}
```

### After — ✓ verified (`-O2`)
```llvm
  store i32 %x, ptr %out
  ret i32 1
```
Both arms were identical, so after sinking the branch has no purpose and is deleted.

A more realistic case is error paths. Ten different `if (!ok) { log(...); cleanup(); return -1; }` blocks often merge into one shared cleanup tail.

**Hardware impact:** this is primarily **code size**, which means I-cache and µop-cache (DSB) footprint. Tail merging can *hurt* hot paths, because it adds a jump and merges branch histories, so compilers avoid it on hot paths when profile data is available.

### Controls
| Control | Effect |
|---|---|
| `-fcrossjumping` / `-fno-crossjumping` (GCC) | Enables or disables RTL tail merging (on at `-O2`) |
| `-mllvm -enable-tail-merge=false` | Disables LLVM's machine-level tail merging |
| `-Os` / `-Oz` | More aggressive merging and outlining |
| `-mllvm -enable-machine-outliner` (AArch64 default at `-Oz`) | Goes further: outlines repeated sequences into shared functions |
| `[[gnu::cold]]` | Marks error paths cold; they get moved out of line and merged freely |

---

## 2.3 Sparse Conditional Constant Propagation (SCCP)

### Mechanism
SCCP combines constant propagation and unreachable-code detection into **one optimistic fixed-point analysis**, and the combination is stronger than running the two separately.

- Each SSA value gets a lattice value: `⊤` (undefined, not yet seen), a **constant**, or `⊥` (overdefined, varies at runtime).
- Each CFG edge is either *executable* or *not yet known*. At first, only the entry edge is executable.
- **Optimism:** a φ meets *only the incoming values from executable edges*. If a back-edge hasn't been proven executable, its value is ignored rather than assumed unknown.
- **Sparseness:** SSA def-use chains mean that when a value's lattice state changes, only its users are revisited. There is no repeated sweep over the whole CFG.
- When a branch condition becomes a constant, only one successor edge is marked executable, so the other side's values never pollute any φ.

This is exactly the worklist + lattice + meet-operator framework in [06_dataflow_analysis.cpp](../../analysis_and_codegen/06_dataflow_analysis.cpp), applied sparsely over SSA edges instead of densely over basic blocks. LLVM's `IPSCCP` runs it **interprocedurally**, propagating constant arguments into callees and constant return values out of them.

### Before
```cpp
int stable(int n) {
    int x = 1;
    for (int i = 0; i < n; ++i) {
        if (x != 1)      // can this ever be true?
            x = 2;
    }
    return x;
}
```

### Why the separate passes fail
Plain constant propagation sees `x = phi [1, entry], [x.next, latch]`, where `x.next` might be `2` from the `if` body. The meet of 1 and "maybe 2" is overdefined, so propagation gives up. Plain DCE can't delete the `if` body without knowing `x == 1`. Each one is waiting on the other.

### How SCCP solves it
1. Start optimistic: `x = phi [1, entry], [⊤, latch]` = **1**, because the back-edge value is not yet known.
2. `x != 1` → `false` → only the "skip" edge is executable, and `x = 2` is never reached.
3. `x.next` on the back-edge is the unchanged `x` = **1**.
4. The φ meets `1 ⊓ 1 = 1`. This is a fixed point and it is self-consistent.

### After — ✓ verified (`-O2`)
```llvm
define noundef i32 @_Z6stablei(i32 noundef %n) {
entry:
  ret i32 1
}
```
The loop is gone too. Once nothing inside it has an effect, it is a side-effect-free finite loop, and it is deleted.

**Hardware impact:** constant-folded branches disappear completely, with no branch-predictor entries and no I-cache bytes. With IPSCCP, a function always called with `mode == 3` can have every `switch (mode)` inside it collapse.

### Controls
| Control | Effect |
|---|---|
| `static` / anonymous-namespace functions | IPSCCP can see *all* call sites only if the function isn't externally visible. Internal linkage is a major enabler |
| `-flto` | Extends IPSCCP across translation units (ch. 7) |
| `-fwhole-program` (GCC) | Assumes the TU is the whole program, so every function is effectively internal |
| `-fipa-cp`, `-fipa-cp-clone` (GCC) | GCC's interprocedural constant propagation and cloning |
| `-ftree-ccp` (GCC) | GCC's conditional constant propagation, on at `-O1` |
| `constexpr` variables | Constants from the start, so SCCP doesn't need to discover them |
