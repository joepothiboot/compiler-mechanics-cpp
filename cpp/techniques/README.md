# cpp/techniques/ — LLVM/MLIR idioms, by family

| Folder | Topic |
|---|---|
| memory_layout | bump allocator, `SmallVector`, pointer+int packing, trailing objects |
| compile_time | X-macros / `.def` files, type traits, SFINAE |
| error_handling | `Expected<T>` without exceptions |
| views_and_callables | `ArrayRef`/`StringRef`, `function_ref` |
| uniquing | string interning, hash-consed types |

Each folder holds standalone files; numbering restarts per folder. Add a folder for a new family — CMake picks it up.
