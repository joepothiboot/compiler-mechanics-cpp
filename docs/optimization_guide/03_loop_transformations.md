# 3. Loop Transformations

Programs spend most of their time in loops, so this is where the optimizer spends most of its effort. Every LLVM loop pass assumes **LoopSimplify form** (one preheader, one latch, one back-edge, dedicated exit blocks) and usually **LCSSA form** (Loop-Closed SSA: every value defined in the loop and used outside it goes through a φ in the exit block). LCSSA means that transforming a loop, by unrolling or versioning it, only requires patching those exit φs, not hunting down every use outside the loop.

SSA gives loops a precise algebraic description. **Scalar Evolution (SCEV)** looks at the header φ, `%i = phi [0, %preheader], [%i.next, %latch]` with `%i.next = add %i, 1`, and derives the closed form `{0,+,1}<%loop>`: "starts at 0, adds 1 per iteration." Trip counts, induction variables, address strides and dependence distances all come from SCEV.

---

## 3.1 Loop-Invariant Code Motion (LICM)

### Mechanism
- An instruction is **loop-invariant** if all its operands are defined outside the loop, or are themselves invariant. In SSA this is a direct check: for each operand, is its defining block inside the loop?
- **Hoisting** moves an invariant instruction to the preheader. It's legal if the instruction is *safe to speculate* (it can't trap: no division by a possibly-zero value, no load from a possibly-invalid pointer) *or* it is guaranteed to execute anyway (its block dominates all loop exits).
- **Sinking** moves instructions whose results are used only after the loop into the exit block.
- **Scalar promotion** is the memory version. A memory location that is loaded and stored every iteration, and provably not aliased by anything else in the loop, is kept in a register and written back once at the exit. This depends entirely on alias analysis (ch. 5).

### Before
```cpp
void scale(float* __restrict__ out, const float* __restrict__ in,
           int n, float s, float k) {
    for (int i = 0; i < n; ++i)
        out[i] = in[i] * (s * k);    // s*k recomputed every iteration?
}
```

### After — ✓ verified (`-O2`, remark: `hoisting fmul`)
```cpp
void scale(float* __restrict__ out, const float* __restrict__ in,
           int n, float s, float k) {
    const float sk = s * k;          // hoisted to the preheader
    for (int i = 0; i < n; ++i)
        out[i] = in[i] * sk;         // then vectorized: 4-wide × 4 interleaved
}
```

### The classic failure: scalar promotion blocked by aliasing
```cpp
void count_positive(const int* v, int n, int* count) {
    for (int i = 0; i < n; ++i)
        if (v[i] > 0) ++*count;   // can't keep *count in a register:
}                                 // count might point INTO v
```
`*count` might alias some `v[i]`, and the store is conditional, so the compiler has to load and store it inside the loop. That puts a store-to-load dependency through memory on the critical path, roughly 4–5 cycles per iteration even with store forwarding.

✓ verified (`-O2`): `loop not vectorized: write to a loop invariant address could not be vectorized`. Rewriting with a local accumulator (`int c = 0; ... if (v[i] > 0) ++c; ... *count = c;`) gives `vectorized loop (vectorization width: 4, interleaved count: 4)`.

**Hardware impact:** hoisting removes work from every iteration, and its real value is that it shortens the loop-carried critical path. Scalar promotion turns a memory round-trip into a register, which is often the difference between a loop that vectorizes and one that can't.

### Controls
| Control | Effect |
|---|---|
| `__restrict__`, local accumulators | Enable scalar promotion |
| `[[gnu::const]]`/`[[gnu::pure]]` on called functions | Let LICM hoist calls |
| `-fno-trapping-math` | Lets FP divisions be speculated, so they can be hoisted |
| `-fno-math-errno` | Makes `sqrt`/`pow` etc. pure, so they can be hoisted |
| `-Rpass=licm` | Reports `hoisting fmul`, `sinking trunc`, … |
| `-fno-tree-loop-im` (GCC) | Disables GCC's LICM |

---

## 3.2 Loop Unrolling

### Mechanism
- The loop body is replicated `U` times, and the induction variable step becomes `U`. A **remainder loop** (or a prologue) handles the `n % U` leftover iterations.
- **Full unrolling:** when SCEV computes a small constant trip count, the loop disappears completely and every iteration becomes straight-line code. That code then gets constant-folded, because every `i` is now a literal.
- **Runtime unrolling:** the trip count is unknown, so the pass unrolls by `U` and adds a remainder loop.
- **Unroll-and-jam:** an outer loop is unrolled and the copies of the inner loop are fused (see §3.5), so the inner body reuses loads across outer iterations.
- In LLVM the *vectorizer's interleave count* is a separate kind of unrolling. `vectorization width: 4, interleaved count: 4` means 4 SIMD registers' worth of independent work per iteration.

### Before
```cpp
static int ipow(int b, int e) {
    int r = 1;
    for (int i = 0; i < e; ++i) r *= b;
    return r;
}
int cube(int b) { return ipow(b, 3); }
```

### After — ✓ verified (`-O2`, remarks: inlined, then `completely unrolled loop with 3 iterations`)
```llvm
define i32 @_Z4cubei(i32 %b) {
  %mul.1 = mul nsw i32 %b, %b
  %mul.2 = mul nsw i32 %mul.1, %b      ; r = 1 * b folded away
  ret i32 %mul.2
}
```
The passes chained: inlining exposed `e = 3`, SCEV computed the trip count of 3, full unrolling produced `1*b*b*b`, and InstCombine folded the `1*`.

**Hardware impact:**
- **Pro:** less loop overhead (fewer IV increments, compares and branches), and multiple *independent* accumulators break the loop-carried dependency so the out-of-order core can overlap iterations. A `sum += a[i]` loop on one accumulator is latency-bound. With 4 accumulators it is throughput-bound.
- **Con:** a bigger body costs I-cache and µop-cache space, and more live values increase register pressure, possibly to the point of spilling. Modern cores also have loop stream detectors that make small *un*-unrolled loops quite cheap. Over-unrolling is a real regression source.

### Controls
| Control | Effect |
|---|---|
| `#pragma clang loop unroll(full)` / `unroll_count(4)` / `unroll(disable)` | Per-loop control in Clang |
| `#pragma GCC unroll 4` | Per-loop control in GCC (Clang accepts it too) |
| `-funroll-loops` | Turns on more aggressive runtime unrolling |
| `-fno-unroll-loops` | Disables unrolling |
| `-mllvm -unroll-threshold=N` | Size budget for unrolling |
| `#pragma clang loop interleave_count(N)` | Sets the vectorizer's interleave factor |

---

## 3.3 Loop Unswitching

### Mechanism
- A loop contains a branch on a **loop-invariant condition**. Unswitching hoists the branch outside the loop and clones the loop, one copy per outcome. Each copy has a branch-free body.
- In SSA this is legal whenever the condition's definition is outside the loop. Cloning is mechanical, and LCSSA tells the pass exactly which exit φs need inputs from both clones.
- **Trivial unswitching** applies when one side of the branch exits the loop. No cloning is needed, and it runs at `-O2`.
- **Non-trivial unswitching** duplicates the whole loop. LLVM's `SimpleLoopUnswitch` enables it **only at `-O3`** because of the size cost.

### Before
```cpp
void log_val(float);

void process(float* a, int n, bool verbose) {
    for (int i = 0; i < n; ++i) {
        a[i] *= 2.0f;
        if (verbose) log_val(a[i]);    // invariant branch, plus an opaque call
    }
}
```

### After — ✓ verified (`-O3`; at `-O2` the loop is not unswitched and not vectorized)
```cpp
void process(float* a, int n, bool verbose) {
    if (verbose) {
        for (int i = 0; i < n; ++i) { a[i] *= 2.0f; log_val(a[i]); }
    } else {
        for (int i = 0; i < n; ++i) a[i] *= 2.0f;   // clean: vectorized 4×4
    }
}
```
The `-O3` IR has `br i1 %verbose` in the loop *preheader* instead of the body, and the `-O3` remarks report the loop as vectorized. At `-O2`, the opaque call `log_val` in the body blocks vectorization.

**Hardware impact:** the main payoff is enabling *other* optimizations, especially vectorization, which can't handle a call in the body. The branch itself is perfectly predictable, so removing it saves little directly. The cost is code size: one full copy of the loop per unswitched condition, and `2^k` copies for `k` conditions, which is why compilers limit it.

### Controls
| Control | Effect |
|---|---|
| `-O3` | Enables non-trivial unswitching (both GCC `-funswitch-loops` and LLVM) |
| Manual unswitch / templates | `template <bool Verbose> void process(...)` with `if constexpr (Verbose)` guarantees it at any `-O` level |
| `-fno-unswitch-loops` (GCC) | Disables it |
| `-mllvm -enable-nontrivial-unswitch` | Forces LLVM's non-trivial unswitching at `-O2` |

---

## 3.4 Loop Fission (Distribution) and Fusion

### Mechanism
**Fission** splits one loop into several loops over the same iteration space.
- It's legal when the dependence graph between the body's statements has no cycle across the split. Statement groups in different strongly-connected components can go in separate loops, in topological order.
- **Why bother:** one part might vectorize and another might not; one part might be a recognizable idiom (`memset`/`memcpy`); or separate loops might each have a smaller working set that fits in cache.

**Fusion** merges adjacent loops with the same trip count into one.
- It's legal when no dependence would be reversed, meaning loop 2 iteration `i` doesn't need a value that loop 1 produces at iteration `j > i`.
- **Why bother:** each array element is loaded once and used while it's still in a register or L1, instead of streaming the whole array through the cache twice.

### Fission: before
```cpp
void init(float* __restrict__ a, float* __restrict__ b, int n) {
    for (int i = 0; i < n; ++i) {
        a[i] = 0.0f;
        b[i] = i * 2.0f;
    }
}
```

### Fission: after — ✓ verified (`-O2`, remark: `Transformed loop-strided store ... into a call to llvm.memset`)
```cpp
void init(float* __restrict__ a, float* __restrict__ b, int n) {
    memset(a, 0, n * sizeof(float));          // split out as an idiom
    for (int i = 0; i < n; ++i) b[i] = i * 2.0f;   // vectorized 4×4
}
```
LLVM's `LoopIdiomRecognize` pulls the `a[i] = 0` store out of the loop into a `memset` call, which is a targeted form of fission. The `memset` implementation uses the widest stores the CPU has and, for large sizes, non-temporal or `DC ZVA` cache-zeroing instructions that a loop wouldn't use. The general `LoopDistribute` pass exists but runs only on request (see the controls below).

### Fusion: before
```cpp
void axpy_then_norm(float* y, const float* x, float a, int n, float& norm) {
    for (int i = 0; i < n; ++i) y[i] += a * x[i];    // pass 1 over y
    norm = 0;
    for (int i = 0; i < n; ++i) norm += y[i] * y[i]; // pass 2 over y
}
```

### Fusion: after (written by hand; see the note below)
```cpp
void axpy_then_norm(float* __restrict__ y, const float* __restrict__ x,
                    float a, int n, float& norm) {
    float acc = 0;
    for (int i = 0; i < n; ++i) {
        float v = y[i] + a * x[i];
        y[i] = v;
        acc += v * v;             // y[i] still in a register
    }
    norm = acc;
}
```
For `n = 10M` floats (40 MB), the unfused version streams `y` from DRAM **twice**. The fused version streams it once, which for a memory-bound loop is close to a 2× speedup.

> **Reality check:** neither GCC nor Clang fuses loops by default at `-O2`/`-O3`. LLVM's `LoopFuse` pass exists but isn't in the default pipeline, and it needs adjacent loops with identical bounds and provably no aliasing. Treat fusion as **something you do by hand** in performance-critical code, or delegate to Polly or GCC Graphite (below), or to frameworks such as Halide and MLIR.

### Controls
| Control | Effect |
|---|---|
| `#pragma clang loop distribute(enable)` | Requests LLVM's `LoopDistribute` for this loop |
| `-mllvm -enable-loop-distribute` | Enables it globally |
| `-ftree-loop-distribution` (GCC) | GCC loop distribution (on at `-O3`) |
| `-ftree-loop-distribute-patterns` (GCC) | memset/memcpy idiom split (default at `-O2`) |
| `-fno-builtin` / `-ffreestanding` | Stops idiom recognition, which matters when *implementing* `memset` |
| `-mllvm -polly` (Polly-enabled LLVM), `-floop-nest-optimize` (GCC + isl) | Polyhedral optimizers that do fusion/fission/tiling together |

---

## 3.5 Loop Interchange

### Mechanism
- Interchange swaps the nesting order of two loops so that the **innermost loop walks memory contiguously**.
- Legality uses **dependence direction vectors**. For each pair of memory accesses, the dependence-analysis framework computes the direction (`<`, `=`, `>`) of the dependence in each loop dimension. Interchange is legal if swapping the loops doesn't produce a vector whose first non-`=` entry is `>`, since that would mean reading a value before it's written.
- SCEV supplies the subscripts: `a[i][j]` with `a` as `float[N][1024]` has address `base + i*4096 + j*4`. A stride of 4 bytes in `j` versus 4096 bytes in `i` tells the cost model which loop should be innermost.

### Before
```cpp
void bump(float (*a)[1024], int n) {
    for (int j = 0; j < 1024; ++j)        // column loop outermost
        for (int i = 0; i < n; ++i)       // inner loop strides by 4 KB!
            a[i][j] += 1.0f;
}
```

### After
```cpp
void bump(float (*a)[1024], int n) {
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < 1024; ++j)    // unit stride → vectorizable
            a[i][j] += 1.0f;
}
```
**Hardware impact:** this is the single biggest cache effect in this guide.
- **Before:** each access touches a *new cache line* (64 B) and uses 4 bytes of it, which is 1/16 utilization. With a 4 KB stride, accesses also map onto the same few L1 cache sets (L1 is typically 8- or 12-way associative), so lines are evicted before their neighbors are used. The TLB thrashes too, with one 4 KB page per access, or one 16 KB page on Apple silicon.
- **After:** sequential access means the hardware prefetcher streams lines ahead, every byte of every line is used, and the loop vectorizes. The difference on large arrays is typically **10×–50×**.

> **Reality check:** LLVM's `LoopInterchange` is **not enabled by default** (it needs `-mllvm -enable-loopinterchange`, and its legality checks often bail on code like the above when the array shape isn't visible). GCC enables `-floop-interchange` at `-O3`. **Write the loop order right yourself.** Row-major C++ arrays want the last index innermost.

### Controls
| Control | Effect |
|---|---|
| Write the last index innermost | The only reliable control |
| `-floop-interchange` (GCC, on at `-O3`) | GCC's interchange |
| `-mllvm -enable-loopinterchange` | LLVM's interchange (off by default) |
| `std::mdspan` (C++23) with `layout_right`/`layout_left` | Makes the layout explicit at the type level |

---

## 3.6 Loop Tiling (Blocking)

### Mechanism
- Each loop is split into an outer "tile" loop and an inner "point" loop, and then the loops are reordered so a small tile of data is fully reused while it sits in cache.
- Formally it's strip-mining plus interchange, so it has the same dependence-vector legality test. Tiling a loop nest is legal when the band of loops is **fully permutable**, meaning all dependence components in the band are non-negative.
- The tile size is chosen so the working set of one tile fits a cache level, e.g. 3 tiles of `T×T` floats ≤ L1 (`T ≈ 32–64`) or ≤ L2 (`T ≈ 128–256`).

### Before
```cpp
void transpose(float* __restrict__ dst, const float* __restrict__ src, int n) {
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            dst[j * n + i] = src[i * n + j];   // src sequential, dst strided by n
}
```

### After
```cpp
void transpose(float* __restrict__ dst, const float* __restrict__ src, int n) {
    constexpr int T = 32;                     // 32×32×4 B = 4 KB per tile
    for (int ii = 0; ii < n; ii += T)
        for (int jj = 0; jj < n; jj += T)
            for (int i = ii; i < std::min(ii + T, n); ++i)
                for (int j = jj; j < std::min(jj + T, n); ++j)
                    dst[j * n + i] = src[i * n + j];
}
```
**Hardware impact:** untiled, every write to `dst` touches a different cache line, and for `n ≥ 1024` those lines are evicted long before the next write to the same line (from the next `i`) arrives. Tiled, a 32×32 block of `dst` touches 32 lines, roughly 2 KB of line footprint, which stays in L1 for all 32 `i` iterations. Each line is fetched once and fully used. For GEMM (`C += A·B`), multi-level tiling (registers → L1 → L2 → L3) is the entire difference between ~1 GFLOP/s naive and ~90% of peak in OpenBLAS, BLIS or Accelerate.

> **Reality check:** mainstream GCC and Clang **do not tile automatically** at `-O3`. Tiling needs a polyhedral optimizer: Polly in LLVM (`-mllvm -polly`, if your clang is built with it; Apple clang isn't) or Graphite in GCC (`-floop-nest-optimize`, if GCC was built with isl). In practice, tile by hand, or call a BLAS, or use a scheduling language (Halide, MLIR `affine`/`linalg` tiling, TVM).

### Controls
| Control | Effect |
|---|---|
| Manual tiling + `constexpr` tile size | Portable and reliable |
| `-floop-nest-optimize` / `-floop-block` (GCC + isl) | Graphite polyhedral tiling |
| `-mllvm -polly -mllvm -polly-tiling` | Polly tiling (Polly-enabled LLVM only) |
| `std::hardware_destructive_interference_size` (C++17 `<new>`) | Portable cache-line size for sizing tiles |
| `__builtin_prefetch(addr, rw, locality)` | Manual prefetch for the next tile |
