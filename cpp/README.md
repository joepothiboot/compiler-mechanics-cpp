# cpp/ — the C++ you need before touching a compiler codebase

Two layers, read in this order:

1. [language/](language/README.md) — ownership, dispatch, and LLVM-style casting. The core semantics.
2. [techniques/](techniques/README.md) — LLVM/MLIR idioms built on top: arenas, `SmallVector`, `Expected`, `ArrayRef`, uniquing.

All C++17 (LLVM's baseline). Next: [../compiler/](../compiler/README.md).
