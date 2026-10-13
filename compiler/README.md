# compiler/ — the pipeline, front to back

Each folder is one stage; run them in order. Each stage assumes the previous stage's output.

| Stage | Files | Input → output |
|---|---|---|
| [01_ir_and_ssa/](01_ir_and_ssa/README.md) | 04–05 | source-like AST → SSA IR (dominators, φ placement) |
| [02_analysis/](02_analysis/README.md) | 06 | SSA IR → dataflow facts (liveness, constants) |
| [03_lowering/](03_lowering/README.md) | 08–09 | SSA IR → machine ops on virtual registers, no φ |
| [04_scheduling/](04_scheduling/README.md) | 11 | machine ops → reordered ops, laid-out blocks |
| [05_register_alloc/](05_register_alloc/README.md) | 07, 10 | virtual registers → physical registers + spills |
| [06_frame_and_peephole/](06_frame_and_peephole/README.md) | 12–13 | allocated code → final frame, local cleanups |

Order constraints worth remembering:
- φ must be eliminated (03) before register allocation (05), which has no notion of φ.
- Liveness (02) feeds register allocation (05).
- Spills create stack slots, so the frame (06) is finalized after allocation.
- Peepholes (06) run last because earlier stages create the patterns they clean up.

Files 08–13 are C++23; 04–07 are C++17. Numbers are reading order from the original sequence, so 07 comes after 10 in the pipeline. Prerequisite: [../cpp/](../cpp/README.md). What the optimizer does with this IR: [../docs/optimization_guide/](../docs/optimization_guide/README.md).
