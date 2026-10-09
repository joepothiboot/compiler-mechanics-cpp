# 1. IR and SSA Cleanups

These passes run early and repeatedly throughout the pipeline. They are cheap, they rarely make code worse, and they expose opportunities for everything else. In LLVM they are mostly carried out by `InstCombine`, `EarlyCSE`, `GVN`, `SCCP`, `ADCE`/`DCE` and `SimplifyCFG`.

**Why SSA matters for all of them.** In SSA, every value has exactly one definition, and every use points directly at that definition (the *use-def chain*). "What is the value of `x` here?" stops being a dataflow question you answer by scanning backward through assignments. It becomes a pointer dereference. Most of the passes below are a single walk over the instructions that follows those pointers.

---

## 1.1 Constant Folding and Constant Propagation

### Mechanism
- **Folding** evaluates an instruction whose operands are all constants at compile time: `add i32 4, 8` → `12`.
- **Propagation** replaces every use of a value known to be constant with that constant. In SSA this is trivial: a value has one definition, so if that definition is a constant, *every* use sees the constant, with no reaching-definitions analysis needed.
- The two feed each other: propagating a constant makes more instructions foldable, and folding them produces more constants. `InstCombine` iterates to a fixed point.
- The `mem2reg`/`SROA` passes do the critical first step. At `-O0`, `int x = 4` is an `alloca` plus `store`/`load`, which nothing can fold through. Promoting it to an SSA register makes it foldable.

### Before
```cpp
int scaled_offset() {
    int x = 4;
    int y = x * 8 + 2;
    return y / 2;
}
```

### After — ✓ verified (`-O2`)
```llvm
define noundef i32 @_Z13scaled_offsetv() {
entry:
  ret i32 17
}
```
**Hardware impact:** the function is a single `mov w0, #17; ret`. No ALU work happens at runtime, and once inlined even the call disappears. The constant becomes an immediate operand in the instruction encoding, so no register is consumed and nothing is loaded from the data cache.

### Controls
| Control | Effect |
|---|---|
| `constexpr` / `consteval` | Guarantees evaluation in the front end (the language requires it); `consteval` makes runtime evaluation a compile error |
| `if constexpr` | Discards the dead branch *before* IR generation, so it is never even type-checked for instantiation |
| `static_assert(f() == 17)` | Proves at compile time that the value is a constant |
| `-O0` | Disables `mem2reg`, so almost nothing folds |
| `-ffp-contract`, `-ffast-math` | Change which floating-point expressions are legal to fold (see ch. 8) |
| `-frounding-math` (GCC) / `#pragma STDC FENV_ACCESS ON` | Forbid folding FP ops whose result depends on the runtime rounding mode |

> **Gotcha:** the optimizer folds `const int k = f();` only if it can see and evaluate `f`. `constexpr` is the contract that makes it *required*; plain `const` makes it merely *possible*.

---

## 1.2 Dead Code Elimination (DCE)

### Mechanism
- An instruction is **dead** if it has no side effects and its result has no uses. SSA tracks uses explicitly (`Value::use_empty()` in LLVM), so DCE is: put every side-effect-free instruction with zero uses on a worklist, delete it, then re-check its operands, which may now have zero uses too.
- **Aggressive DCE (ADCE)** inverts the logic: it assumes everything is dead, marks instructions *live* starting from roots (stores, calls with side effects, returns, terminators that matter), and deletes whatever is never marked. This removes cycles of mutually-used but useless values, such as a dead loop induction variable whose φ uses itself.
- **Dead store elimination (DSE)** is the memory version: a store overwritten before any possible read is deleted. It needs alias analysis (ch. 5).
- Dead *blocks* (unreachable code) are removed by `SimplifyCFG` once a branch condition folds to a constant.

### Before
```cpp
int checksum(const int* data, int n, bool debug) {
    int sum = 0;
    int calls = 0;            // only ever written, never read
    for (int i = 0; i < n; ++i) {
        sum += data[i];
        ++calls;              // dead: φ-cycle with no external use
    }
    int tmp = sum * 3;        // dead: never used
    (void)tmp;
    return sum;
}
```

### After
```cpp
int checksum(const int* data, int n, bool /*debug*/) {
    int sum = 0;
    for (int i = 0; i < n; ++i) sum += data[i];
    return sum;
}
```
In SSA, `calls` is `%calls = phi [0, %entry], [%calls.next, %loop]` and `%calls.next = add %calls, 1`. Each instruction "uses" the other, so plain DCE keeps both. ADCE never marks either one live, because nothing that matters depends on them.

**Hardware impact:** fewer instructions means fewer µops dispatched, fewer physical registers held in the loop, and a smaller I-cache footprint. In a hot loop, removing one dead increment can free a register and avoid a spill.

### Controls
| Control | Effect |
|---|---|
| `volatile` | Every access is a side effect, so DCE must keep it. Benchmarking code abuses this |
| `benchmark::DoNotOptimize(x)` / `asm volatile("" : : "r"(x) : "memory")` | Creates an opaque use of `x`, so DCE can't remove it |
| `[[maybe_unused]]` | Silences the warning only, not the optimization |
| `-ffunction-sections -fdata-sections -Wl,--gc-sections` (ELF) / `-Wl,-dead_strip` (Mach-O) | Link-time DCE of whole unreferenced functions |
| `__attribute__((used))` | Keeps a symbol alive even if nothing references it |

> **Gotcha:** a microbenchmark that computes a result and never uses it measures nothing. The whole loop is dead, and DCE removes it.

---

## 1.3 Common Subexpression Elimination (CSE)

### Mechanism
- Two instructions are **equivalent** if they have the same opcode and the same operands, and they are pure. In SSA, "the same operand" means *the same SSA value*, compared by pointer identity. No reaching-definitions check is needed, because SSA values never change after definition.
- **Local / Early CSE** walks the dominator tree with a scoped hash table. If an equivalent instruction is already in scope (it dominates this one), the pass replaces this one with it. Dominance is what makes the replacement legal: the earlier value is guaranteed to have been computed on every path that reaches here.
- Loads can be CSE'd only if no intervening instruction may write that memory. That requires the alias analysis from ch. 5.

### Before
```cpp
int area_ratio(int a, int b) {
    return (a * b + 7) * (a * b + 7);
}
```

### After — ✓ verified (`-O2`)
```llvm
  %mul  = mul nsw i32 %b, %a
  %add  = add nsw i32 %mul, 7
  %mul3 = mul nsw i32 %add, %add      ; one multiply, reused, not four
  ret i32 %mul3
```
Note `%b, %a`: the operands were also **canonicalized** (§1.5), so `a*b` and `b*a` hash identically.

**Hardware impact:** an integer multiply has 3–4 cycles of latency on modern cores. Removing a redundant one shortens the dependency chain and frees a multiplier port.

### Controls
| Control | Effect |
|---|---|
| `[[gnu::const]]`, `[[gnu::pure]]` | Let the optimizer CSE calls to opaque functions (see ch. 7) |
| `-fno-math-errno` | `sqrt(x)` no longer writes `errno`, so repeated calls can be CSE'd |
| `volatile` | Every read is distinct, so CSE is blocked |
| `-fno-gcse` (GCC) | Disables global CSE (debugging only) |

---

## 1.4 Global Value Numbering (GVN)

### Mechanism
- CSE asks "is this the *same instruction*?" **GVN** asks "does this compute the *same value*?" It assigns a **value number** to every expression, and expressions with equal numbers are interchangeable, even if they are syntactically different or sit in different blocks.
- Value numbers propagate through copies and φs: if `y = x` and `z = x + 1`, then `y + 1` gets the same number as `z`.
- LLVM's `GVN` pass also does **load elimination** with `MemorySSA` / `MemoryDependenceAnalysis`. A load whose value is available from an earlier store or load, on every path, is replaced. If the value is available on only *some* paths, GVN does **PRE** (partial redundancy elimination): it inserts the computation on the missing paths and places a φ at the merge.
- `NewGVN` is LLVM's alternative implementation (`-mllvm -enable-newgvn`). It uses optimistic congruence classes and can discover equivalences through φ cycles that the default GVN misses.

### Before
```cpp
struct Particle { float x, y, vx, vy; };

float step(Particle& p, float dt, bool damp) {
    float nx = p.x + p.vx * dt;
    if (damp) p.vy *= 0.9f;
    float ny = p.y + p.vy * dt;        // p.vy: reload, or the value just stored?
    return nx + ny + p.vx;             // p.vx: reload?
}
```

### After — ✓ verified (`-O2`, remark: `load eliminated by PRE`)
```llvm
entry:
  %0 = load float, ptr %p            ; p.x
  %1 = load float, ptr %vx           ; p.vx: loaded ONCE, reused in the return
  %2 = load float, ptr %vy           ; p.vy
  br i1 %damp, label %if.then, label %if.end
if.then:
  %mul = fmul float %2, 0x3FECCCCCC0000000
  store float %mul, ptr %vy
  br label %if.end
if.end:
  %3 = phi float [ %mul, %if.then ], [ %2, %entry ]   ; PRE: no reload of p.vy
  %4 = call float @llvm.fmuladd.f32(float %1, float %dt, float %0)
  %5 = load float, ptr %y
  %6 = call float @llvm.fmuladd.f32(float %3, float %dt, float %5)
  ...
```
GVN proves that `p.vx` is unchanged between its two uses, because the only store is to `p.vy`, a different field offset of the same object. The second load becomes `%1`. For `p.vy`, the value is available on both incoming paths but from *different* sources: the stored `%mul` on one, the original load `%2` on the other. PRE replaces the reload with a φ that merges them.

**Hardware impact:** each eliminated load saves an L1 hit (4–5 cycles of latency, plus a load-port slot). More importantly, a load that might alias an earlier store forces the CPU's memory disambiguation logic to speculate, and a misprediction triggers a pipeline flush. Values held in registers have none of that risk.

### Controls
| Control | Effect |
|---|---|
| `-fstrict-aliasing` (default at `-O2`) | Lets TBAA prove loads independent, so GVN can eliminate more of them |
| `-fno-strict-aliasing` | Assumes any pointer may alias any other, which blocks most load elimination |
| `__restrict__` | Promises no aliasing (ch. 5) |
| `-mllvm -enable-newgvn` | Uses NewGVN instead of GVN |
| `-Rpass=gvn` | Reports e.g. `load of type float eliminated` |

> **Gotcha:** `x * 0.0f` is *not* folded to `0` without `-fno-signed-zeros -ffinite-math-only` (both implied by `-ffast-math`): `-1.0f * 0.0f == -0.0f` and `inf * 0.0f == NaN`. Integer `x * 0` *is* always folded.

---

## 1.5 Canonicalization

### Mechanism
Many different expressions mean the same thing. Canonicalization rewrites each one into **one standard form**, so that later pattern-matching passes (CSE, GVN, LICM, the vectorizer, instruction selection) only need to recognize one shape. LLVM's `InstCombine` does most of this. Its canonical forms include:

| Input | Canonical form | Why |
|---|---|---|
| `7 + x` | `x + 7` | Constants go on the right |
| `b * a` and `a * b` | operands ordered by "complexity" rank | CSE and GVN hash them identically |
| `x * 8` | `x << 3` | One canonical form for power-of-2 multiplies |
| `x - 5` | `x + (-5)` | One add form instead of two |
| `!(a < b)` | `a >= b` | Removes the `xor` |
| `if (c) x = a; else x = b;` (simple) | `select i1 %c, a, b` | Branch-free form that the vectorizer and if-conversion recognize |
| `for (; i != n; ...)` | rotated loop with a guard plus a bottom test | `LoopRotate` gives every loop a single canonical shape |
| any loop | **LoopSimplify form**: one preheader, one latch, dedicated exits | Required by LICM, unrolling and vectorization |

A φ with a single incoming value, or where all incoming values are the same, is replaced by that value. This is exactly the trivial-φ removal in [05_ssa_construction.cpp](../../ir_and_ssa/05_ssa_construction.cpp).

### Before
```cpp
bool in_range(unsigned v) {
    return !(v < 10) && !(v > 20);
}
int idx(int i) { return 4 + i * 8; }
```

### After — ✓ verified for `in_range` (`-O2`)
```llvm
; in_range: the negations are folded, then the two compares merge into ONE unsigned range check
%0 = add i32 %v, -10          ; "v - 10" canonicalized to "v + (-10)"
%1 = icmp ult i32 %0, 11      ; (v - 10) <u 11   <=>   10 <= v <= 20
; idx: multiply by 8 canonicalized to a shift, constant on the right
%shl = shl i32 %i, 3
%add = add i32 %shl, 4
```
**Hardware impact:** indirect but large. Canonical forms are what `-O2` pattern-matches into efficient instructions: `x << 3` plus an add becomes one address-mode operation (`lea` on x86, `add x0, x1, x2, lsl #3` on AArch64), and `select` becomes `csel`/`cmov` with no branch to mispredict.

### Controls
There is no direct knob, since canonicalization is always on at `-O1` and above. Help it by:
- Writing straightforward code. Hand-"optimized" bit tricks often *defeat* pattern matching. For example, a manual `(x >> 31) ^ x` abs can miss the target's native `abs`/`cneg` instruction.
- Using `std::abs`, `std::min`, `std::max`, `std::clamp`, `std::popcount`, `std::rotl` (C++20 `<bit>`). They map to canonical intrinsics such as `llvm.smin`, `llvm.ctpop` and `llvm.fshl`.
