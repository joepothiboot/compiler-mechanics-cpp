# compiler-mechanics-cpp

A focused C++ study repository for engineers transitioning into compiler engineering (LLVM/MLIR-adjacent roles). Each file demonstrates **one** compiler-relevant C++ technique — with working code, real tests, and comments explaining *why the technique matters in compiler engineering specifically*, not generic C++ tutorial content.

## Why this exists

Frontend/backend engineers moving into systems roles usually know modern C++ syntax but haven't internalized the *ownership models*, *dispatch strategies*, and *IR-representation idioms* that dominate real compiler codebases. This repo closes that gap with seven self-contained, compilable files instead of one giant reference — so each concept can be read, run, and modified in isolation.

## C++ Standard: C++17

LLVM and MLIR's coding standards require **C++17** as the baseline (a proposal to raise it to C++20 exists but hasn't landed as the enforced default). Writing C++17 here — structured bindings, `if`-init statements, `std::variant`, CTAD, fold-expression-adjacent patterns, but *no* concepts/ranges/coroutines — builds the exact muscle memory you'll use on day one in an LLVM/MLIR codebase.

---

## Repository layout

```
compiler-mechanics-cpp/
├── CMakeLists.txt
├── test_support.h
├── cpp_language/
│   ├── 01_memory_ownership.cpp
│   ├── 02_polymorphism.cpp
│   └── 03_rtti_isa_dyncast.cpp
├── ir_and_ssa/
│   ├── 04_ir_data_structures.cpp
│   └── 05_ssa_construction.cpp
└── analysis_and_codegen/
    ├── 06_dataflow_analysis.cpp
    └── 07_register_allocation.cpp
```

No external dependencies — standard library only. Tests use a lightweight `CHECK`/`assert`-based harness (`test_support.h`), not a full test framework, so the whole repo builds with just a compiler.

---

## What each file covers

### `test_support.h`
A minimal check harness (`CHECK(cond)`, `CHECK_MSG(cond, msg)`) that counts failures and reports pass/fail per file. No dependency beyond `<cstdio>`. Mirrors the "assert aggressively" culture of LLVM's assertions-enabled development builds.

### `01_memory_ownership.cpp` — Memory and ownership
- `unique_ptr` + move semantics, including **why you can still assign into a moved-from `unique_ptr`** (move-assignment is specified to leave the source holding `nullptr` — it's alive, just empty).
- A context/arena ownership pattern (`IRContext`) modeling how `MLIRContext`/`LLVMContext` own all IR nodes while everything else holds raw, non-owning pointers.
- An RAII `InsertionGuard`, modeled on `mlir::OpBuilder::InsertionGuard`, showing scope-based save/restore.
- `shared_ptr` reference-counting mechanics **and a reference-cycle leak**, followed by the `weak_ptr` fix — explaining why LLVM/MLIR avoid `shared_ptr` for IR almost entirely.

### `02_polymorphism.cpp` — Type system and dispatch
- Classic virtual dispatch (vtables), including the constructor trap (virtual calls in a base constructor resolve to the base override).
- **Why a member template can't be virtual** — vtables need statically-fixed slots, but template instantiations are unbounded — and what compilers do instead (double dispatch, CRTP, or kind-tag switches).
- A double-dispatch visitor (classic `accept`/`visit` pattern).
- CRTP (Curiously Recurring Template Pattern) used exactly like `llvm::InstVisitor` — static dispatch with a delegation chain (`visitAdd → visitBinary → visitDefault`), plus a `PassInfoMixin`-style interface-injection example.

### `03_rtti_isa_dyncast.cpp` — Casting without RTTI
A minimal reimplementation of LLVM's `isa<>` / `cast<>` / `dyn_cast<>` / `dyn_cast_or_null<>` family, using a `classof` convention with **contiguous kind-enum ranges** (exactly how `llvm/IR/Value.def` is structured). Includes a `dynamic_cast` comparison block that's conditionally compiled out under `-fno-rtti`, demonstrating why LLVM ships this idiom instead of relying on built-in RTTI (binary size, inlining, and buildability without `-frtti`).

### `04_ir_data_structures.cpp` — IR-relevant data structures
- An **intrusive doubly-linked list** (the pattern behind `Instruction`/`BasicBlock` lists in LLVM), including O(1) self-removal (`eraseFromParent`) and O(1) splicing across blocks (`moveBefore`) — with an allocation counter proving it does fewer allocations than `std::list<T*>` + a side-table of iterators.
- A `std::variant`-based AST node (`NumLit` / `VarRef` / `BinOp` / `LetExpr`) with `std::visit` and the `overloaded{}` lambda-pack trick, including an interpreter and a constant-folding rewrite pass — showing the "closed tagged union" alternative to inheritance-based IR.

### `05_ssa_construction.cpp` — SSA construction, by hand
The full Cytron et al. algorithm, implemented without LLVM:
1. CFG + predecessor construction
2. Iterative dominator computation (Cooper–Harvey–Kennedy)
3. Dominance frontier computation
4. Phi-node placement at the iterated dominance frontier
5. Stack-based renaming (dominator-tree DFS)
6. Dead-phi elimination (minimal → pruned SSA)

Tested on a counted loop and an if/else diamond, with an **interpreter that runs both the pre-SSA and post-SSA forms and checks they produce identical results** across multiple inputs — proving the transformation preserves semantics rather than just "looking right."

### `06_dataflow_analysis.cpp` — Dataflow analysis
A generic bitvector-based worklist solver, instantiated twice:
- **Live-variable analysis** (backward, meet = union) — the analysis that feeds register allocation.
- **Reaching-definitions analysis** (forward, meet = union) — the analysis behind constant propagation and def-use chains.

Both run on the same hand-built loop CFG, with hand-verified expected fixpoints asserted in the tests.

### `07_register_allocation.cpp` — Graph-coloring register allocation
A Chaitin–Briggs-style allocator: simplify (remove degree-`< K` nodes), optimistic spill (push a high-degree node speculatively rather than giving up), and select (pop and color, or mark a real spill if no color remains). Includes:
- Precolored nodes (modeling fixed physical registers like `%eax`)
- A test proving Briggs' *optimistic* spilling succeeds on a 4-cycle where naive Chaitin coloring would give up
- A live-range → interference-graph builder, connecting this file back to the liveness analysis in file 06
- A `verify()` function asserting no two interfering registers ever share a color

---

## How to run it locally

### Option A — CMake (builds and tests everything at once)

```bash
git clone https://github.com/<your-username>/compiler-mechanics-cpp.git
cd compiler-mechanics-cpp

cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Each topic file becomes its own executable (`01_memory_ownership`, `02_polymorphism`, etc.) and its own `ctest` entry, so you can see pass/fail per topic.

### Option B — One file at a time, no CMake required

```bash
g++ -std=c++17 -Wall -Wextra -g -I. ir_and_ssa/05_ssa_construction.cpp -o ssa
./ssa
```

Repeat for any file. Useful when you're only studying one topic and want a fast edit-compile-run loop.

### Option C — Sanitizers (recommended for file 04, which does manual pointer/list surgery)

```bash
g++ -std=c++17 -g -fsanitize=address,undefined -I. ir_and_ssa/04_ir_data_structures.cpp -o ir
./ir
```

### Verifying the RTTI-avoidance claim in file 03

The CMake build also compiles `03_rtti_isa_dyncast.cpp` a second time with `-fno-rtti` to prove the `isa<>`/`dyn_cast<>` machinery works without compiler RTTI support (the `dynamic_cast` comparison block compiles out automatically). To do it manually:

```bash
g++ -std=c++17 -fno-rtti -I. cpp_language/03_rtti_isa_dyncast.cpp -o isa_no_rtti
./isa_no_rtti
```

---

## Suggested reading order

1. `01_memory_ownership.cpp` → `02_polymorphism.cpp` → `03_rtti_isa_dyncast.cpp` — the C++-specific language gap (ownership + dispatch + casting).
2. `04_ir_data_structures.cpp` — how IR nodes are actually stored in memory.
3. `05_ssa_construction.cpp` — the highest-leverage file; try deleting the dead-phi elimination step and see which φ node survives, to feel the difference between minimal and pruned SSA.
4. `06_dataflow_analysis.cpp` → `07_register_allocation.cpp` — these connect directly: liveness output becomes the register-interference graph.

## License

MIT (or your preference — add a `LICENSE` file before pushing if you want this reusable by others).

---

Want me to also draft the actual `LICENSE` file text, or a short `.gitignore` for a CMake C++ project (build/, CMakeCache.txt, etc.) so the repo is clean on first push?