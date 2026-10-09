# compiler/ — the pipeline, front to back

1. [ir_and_ssa/](ir_and_ssa/README.md) — represent code, then put it in SSA form.
2. [analysis_and_codegen/](analysis_and_codegen/README.md) — dataflow analysis and register allocation.
3. [backend/](backend/README.md) — leave SSA, select instructions, schedule, build frames, peephole. **C++23.**

Files are numbered 04–13 in reading order. Prerequisite: [../cpp/](../cpp/README.md). What the optimizer does with this IR: [../docs/optimization_guide/](../docs/optimization_guide/README.md).
