# 8. Hardware-Level Transformations

The last group sits where IR meets silicon. **FMA contraction** is an IR-level decision whose legality comes from the language's floating-point rules, and it's realized by instruction selection. **Cache locality and alignment** are mostly *not* things the compiler can fix for you: it rarely changes data layout, because the layout of a C++ type is part of the ABI. You choose the layout, and the compiler and CPU reward or punish the choice.

---

## 8.1 Fused Multiply-Add (FMA) Contraction

### Mechanism
- `a * b + c` is normally two IEEE operations, each rounded: `round(round(a*b) + c)`. An **FMA** instruction computes `round(a*b + c)` with a *single* rounding. It's faster (one instruction, ~4 cycles instead of ~3 + 3) and *more* accurate, but **its result can differ in the last bit**. A compiler may only fuse the two operations if the language permits it.
- The C/C++ standards permit contraction *within one expression* (`#pragma STDC FP_CONTRACT`). Clang models this precisely: the front end emits `@llvm.fmuladd` ("fused or not, the backend decides") only for a multiply and add that appear **in the same source expression**. Separate statements produce plain `fmul` + `fadd`, which may be fused only with `-ffp-contract=fast` (or `fast`/`contract` fast-math flags on the instructions).
- In SSA, contraction is a local pattern match at instruction selection: an `fadd` whose operand is an `fmul` with a single use, and that is allowed to contract, becomes one `fmadd` machine instruction.

### Before
```cpp
float f(float a, float b, float c) { return a * b + c; }              // one expression
float g(float a, float b, float c) { float t = a * b; return t + c; } // two statements
```

### After — ✓ verified (Apple clang 21, `-O2`)
| Flags / target | `f` | `g` |
|---|---|---|
| arm64, default (`-ffp-contract=on`) | `fmadd` | `fmul` + `fadd` |
| arm64, `-ffp-contract=off` | `fmul` + `fadd` | `fmul` + `fadd` |
| arm64, `-ffp-contract=fast` | `fmadd` | `fmadd` |
| x86-64, default (baseline SSE2 has no FMA) | `mulss` + `addss` | — |
| x86-64, `-mfma` or `-march=x86-64-v3` | `vfmadd213ss` | — |

The IR for `f` is `call float @llvm.fmuladd.f32(float %a, float %b, float %c)`.

Note that **GCC's default differs**: for C++, GCC uses `-ffp-contract=fast`, so it fuses across statements too, whatever `-std` you pass. Only *C* in strict ISO mode (`-std=c11` etc.) defaults to `off`. The same C++ source can give bit-different results under GCC and Clang.

**Hardware impact:**
- **Throughput:** modern cores issue 2 FMAs per cycle per FMA unit, each doing a multiply and an add. Peak FLOP/s figures assume FMA, so a dot product or matmul without it runs at half speed.
- **Latency:** one ~4-cycle op instead of two dependent ops (~6–8 cycles). In a reduction chain like `acc = acc + x[i]*y[i]`, the loop-carried latency roughly halves.
- **Accuracy:** single rounding is usually better, but it breaks bit-reproducibility *and* some algorithms that rely on exact rounding, e.g. `a*b - a*b` may be nonzero when contracted (one product rounded, the other fused). Error-free transforms (TwoSum, Dekker) and some geometry predicates need `-ffp-contract=off`.

### Controls
| Control | Effect |
|---|---|
| `-ffp-contract=on` | Contract within an expression (Clang's default for C/C++) |
| `-ffp-contract=fast` | Contract across statements (GCC's default for C++; implied by `-ffast-math`) |
| `-ffp-contract=off` | Never contract, for bit-exact reproducibility |
| `#pragma STDC FP_CONTRACT ON/OFF`, `#pragma clang fp contract(fast/on/off)` | Scoped control |
| `std::fma(a, b, c)` | **Always** a fused op, with no permission needed (a library call if the target lacks FMA) |
| `-mfma`, `-march=haswell`/`x86-64-v3`, `-march=native` | Needed on x86-64. **Baseline x86-64 has no FMA** |
| AArch64 | FMA is always available (`fmadd`/`fmla`) |

---

## 8.2 Cache Locality: Layout, Alignment and Access Patterns

### The hardware model
- Memory moves in **cache lines**: 64 B on x86 and most ARM cores, **128 B on Apple M-series** (`sysctl hw.cachelinesize` → `128` on this machine).
- L1 hit ≈ 4 cycles, L2 ≈ 12–16, L3 ≈ 40–80, DRAM ≈ 200–400 cycles. One cache miss costs about as much as hundreds of arithmetic ops.
- **Hardware prefetchers** detect sequential and fixed-stride streams and fetch ahead. Random, pointer-chasing access defeats them.
- Lines are shared between cores with a coherence protocol (MESI and variants). Two cores *writing* to the same line ping-pong it between their caches.

The compiler's role is limited. It **can** reorder loops (ch. 3), vectorize with aligned or unaligned loads, insert prefetches (`-fprefetch-loop-arrays` on GCC, rarely profitable), and align functions and loops. It **cannot** change a `struct`'s layout or how containers store objects. **Those decisions are yours.**

---

### 8.2.1 Array-of-Structs vs Struct-of-Arrays

#### Before (AoS)
```cpp
struct Particle { float x, y, z; float vx, vy, vz; float mass; int flags; };  // 32 B
std::vector<Particle> ps;

void advance(std::vector<Particle>& ps, float dt) {
    for (auto& p : ps) p.x += p.vx * dt;   // uses 8 B out of every 32 B loaded
}
```

#### After (SoA)
```cpp
struct Particles {
    std::vector<float> x, y, z, vx, vy, vz, mass;
    std::vector<int>   flags;
};
void advance(Particles& ps, float dt) {
    float* __restrict__ x = ps.x.data();
    const float* __restrict__ vx = ps.vx.data();
    for (size_t i = 0, n = ps.x.size(); i < n; ++i) x[i] += vx[i] * dt;
}
```
**Why it's faster:**
- **Cache line utilization:** AoS loads 32 B per particle to use 8 B (25%). SoA uses 100% of every line it loads, so the loop moves 4× less data for memory-bound sizes.
- **Vectorization:** AoS `p.x` values are 32 B apart, so a vector needs a strided gather, which NEON lacks and which is slow on AVX2. SoA's `x[i..i+3]` is one contiguous 16-byte load. The SoA loop vectorizes cleanly (see the `saxpy`-style results in ch. 4). The AoS loop typically doesn't, or only with costly shuffles.
- **Hybrid AoSoA** (`struct Block { float x[8], vx[8], …; }`) keeps SIMD-width chunks together. It's common in game engines and physics codes when one entity's fields are used together.

### 8.2.2 Field ordering and padding
```cpp
struct Bad  { char tag; double value; char kind; int count; };   // 24 B: 7 B + 3 B padding
struct Good { double value; int count; char tag; char kind; };   // 16 B: same data
```
Ordering fields from largest to smallest alignment removes padding, so more objects fit per cache line. Put **hot fields together at the front** and move cold fields (debug names, rarely-used metadata) to the end or out of line (`std::unique_ptr<ColdData>`). Check with `static_assert(sizeof(Good) == 16)`, or `-Wpadded` (warns on every padding insertion; noisy).

### 8.2.3 False sharing

#### Before
```cpp
struct Counters {
    std::atomic<long> a{0};   // written by thread 1
    std::atomic<long> b{0};   // written by thread 2: same cache line!
};
```

#### After
```cpp
struct Counters {
    alignas(128) std::atomic<long> a{0};   // each on its own line
    alignas(128) std::atomic<long> b{0};
};
```
✓ measured on this machine (Apple silicon, `-O2`, two threads each doing 50M relaxed `fetch_add`s):

| Layout | Time |
|---|---|
| `a` and `b` adjacent (same line) | **483 ms** |
| `alignas(128)` (separate lines) | **105 ms** (4.6× faster) |

The threads never touch the same *variable*, but coherence works per *line*. Each write invalidates the other core's copy, so the line bounces between cores on every increment.

`std::hardware_destructive_interference_size` (C++17 `<new>`) is the portable constant. ✓ verified: libc++ here reports `destructive = 256`, `constructive = 64`. It's conservative: it covers adjacent-line prefetching and differs from the actual 128-byte line. Because the value can vary between compiler versions and flags, using it in a type that crosses an **ABI** boundary is risky (GCC warns with `-Winterference-size`). Hard-coding 64 (x86) or 128 (Apple M-series, some ARM servers) is common practice.

### 8.2.4 Alignment for SIMD

#### Mechanism
- Modern cores handle *unaligned* vector loads at full speed **unless they cross a cache line**. A line-split load costs roughly double, and a load that crosses a 4 KB **page boundary** is much slower still.
- The vectorizer emits unaligned loads by default (`align 4` in IR for a `float*`), so it is always correct. If it can prove 16, 32 or 64-byte alignment, it may use aligned instructions (`vmovaps` on x86) and skip peeling.
- For streaming kernels with AVX-512 (64-byte vectors), unaligned data means *every* load is a line split. Aligning the buffer to 64 B can give ~10–20% on bandwidth-bound loops.

#### Before
```cpp
float* buf = static_cast<float*>(std::malloc(n * sizeof(float)));  // 16-B aligned (typically)
```

#### After
```cpp
// C++17 aligned allocation
float* buf = static_cast<float*>(std::aligned_alloc(64, round_up(n * sizeof(float), 64)));
// or: a std::vector with an aligned allocator; or `new (std::align_val_t{64}) float[n]`

void kernel(float* __restrict__ p, size_t n) {
    p = std::assume_aligned<64>(p);          // C++20: promise to the optimizer
    for (size_t i = 0; i < n; ++i) p[i] *= 2.0f;
}
```

#### Controls
| Control | Effect |
|---|---|
| `alignas(N)` on types and variables | Static alignment. Raises the type's alignment and size to multiples of N |
| `std::aligned_alloc`, `operator new(size, std::align_val_t)` | Aligned heap allocation (C++17). Note: `aligned_alloc`'s size must be a multiple of the alignment |
| `std::assume_aligned<N>(p)` (C++20), `__builtin_assume_aligned(p, N)` | Tells the optimizer that the alignment holds, without changing anything. **UB if false** |
| `-falign-functions=N`, `-falign-loops=N` | Code alignment, so hot loop heads don't straddle fetch blocks |
| `-mllvm -align-all-nofallthru-blocks=N` | Fine-grained code alignment (LLVM; `N` is log2 of the byte alignment) |
| `__builtin_prefetch(p, rw, locality)` | Manual software prefetch, for irregular-but-predictable access such as linked structures or gathers |
| `[[no_unique_address]]` (C++20) | Lets empty members take no space |
| `-Wpadded` | Reports padding |
| `pahole` (Linux, dwarves), `clang -Xclang -fdump-record-layouts` | Prints actual struct layouts with holes |

---

## Summary: what the compiler does and what you do

| Concern | Compiler handles it | You must handle it |
|---|---|---|
| FMA | Contracts within expressions (and across statements with `-ffp-contract=fast`) | Enable `-mfma`/`-march` on x86. Choose reproducibility vs speed |
| Loop order | Sometimes (GCC `-O3` interchange) | Write the last index innermost |
| Tiling | No (without Polly/Graphite) | Block manually, or use BLAS/Halide/MLIR |
| AoS → SoA | **Never** (layout is ABI) | Design data for the access pattern |
| Padding / field order | **Never** (layout is ABI) | Order fields, split hot and cold |
| False sharing | **Never** | `alignas(cache line)` per-thread data |
| Buffer alignment | Uses whatever alignment it can prove | `aligned_alloc` + `assume_aligned` |
