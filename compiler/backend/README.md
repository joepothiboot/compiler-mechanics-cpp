# compiler/backend/ — SSA to machine code (C++23)

Uses `std::format`, `std::ranges::contains`, designated initializers, `std::countr_zero`; needs libc++ ≥ 19 or GCC 13+.

| File | Topic |
|---|---|
| 08_phi_elimination | leaving SSA |
| 09_instruction_selection | IR to machine ops |
| 10_live_intervals_linear_scan | live intervals, linear-scan allocation with spilling |
| 11_scheduling_and_layout | instruction scheduling, block layout |
| 12_stack_frame_abi | frame layout and calling convention |
| 13_peephole | local cleanups |
