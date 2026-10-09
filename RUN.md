# Run commands

Run from the repo root. Binaries go to `build/bin/`.

## Everything (CMake)

```sh
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```

## Everything (plain g++ loop)

```sh
mkdir -p build/bin
for f in $(find . -name '*.cpp' -not -path './build/*' | sort); do
  std=c++17; case $f in ./compiler/backend/*) std=c++23;; esac
  n=$(echo ${f#./} | tr / _ | sed 's/\.cpp$//')
  g++ -std=$std -Wall -Wextra -g -I. $f -o build/bin/$n && build/bin/$n || echo "FAILED: $f"
done
```

## One file each

```sh
g++ -std=c++17 -Wall -Wextra -g -I. compiler/analysis_and_codegen/06_dataflow_analysis.cpp -o build/bin/analysis_and_codegen_06_dataflow_analysis && build/bin/analysis_and_codegen_06_dataflow_analysis
```

```sh
g++ -std=c++17 -Wall -Wextra -g -I. compiler/analysis_and_codegen/07_register_allocation.cpp -o build/bin/analysis_and_codegen_07_register_allocation && build/bin/analysis_and_codegen_07_register_allocation
```

```sh
g++ -std=c++23 -Wall -Wextra -g -I. compiler/backend/08_phi_elimination.cpp -o build/bin/backend_08_phi_elimination && build/bin/backend_08_phi_elimination
```

```sh
g++ -std=c++23 -Wall -Wextra -g -I. compiler/backend/09_instruction_selection.cpp -o build/bin/backend_09_instruction_selection && build/bin/backend_09_instruction_selection
```

```sh
g++ -std=c++23 -Wall -Wextra -g -I. compiler/backend/10_live_intervals_linear_scan.cpp -o build/bin/backend_10_live_intervals_linear_scan && build/bin/backend_10_live_intervals_linear_scan
```

```sh
g++ -std=c++23 -Wall -Wextra -g -I. compiler/backend/11_scheduling_and_layout.cpp -o build/bin/backend_11_scheduling_and_layout && build/bin/backend_11_scheduling_and_layout
```

```sh
g++ -std=c++23 -Wall -Wextra -g -I. compiler/backend/12_stack_frame_abi.cpp -o build/bin/backend_12_stack_frame_abi && build/bin/backend_12_stack_frame_abi
```

```sh
g++ -std=c++23 -Wall -Wextra -g -I. compiler/backend/13_peephole.cpp -o build/bin/backend_13_peephole && build/bin/backend_13_peephole
```

```sh
g++ -std=c++17 -Wall -Wextra -g -I. cpp/language/01_memory_ownership.cpp -o build/bin/cpp_language_01_memory_ownership && build/bin/cpp_language_01_memory_ownership
```

```sh
g++ -std=c++17 -Wall -Wextra -g -I. cpp/language/02_polymorphism.cpp -o build/bin/cpp_language_02_polymorphism && build/bin/cpp_language_02_polymorphism
```

```sh
g++ -std=c++17 -Wall -Wextra -g -I. cpp/language/03_rtti_isa_dyncast.cpp -o build/bin/cpp_language_03_rtti_isa_dyncast && build/bin/cpp_language_03_rtti_isa_dyncast
```

```sh
g++ -std=c++17 -Wall -Wextra -g -I. cpp/techniques/compile_time/01_x_macros.cpp -o build/bin/cpp_techniques_compile_time_01_x_macros && build/bin/cpp_techniques_compile_time_01_x_macros
```

```sh
g++ -std=c++17 -Wall -Wextra -g -I. cpp/techniques/compile_time/02_type_traits_sfinae.cpp -o build/bin/cpp_techniques_compile_time_02_type_traits_sfinae && build/bin/cpp_techniques_compile_time_02_type_traits_sfinae
```

```sh
g++ -std=c++17 -Wall -Wextra -g -I. cpp/techniques/error_handling/01_expected.cpp -o build/bin/cpp_techniques_error_handling_01_expected && build/bin/cpp_techniques_error_handling_01_expected
```

```sh
g++ -std=c++17 -Wall -Wextra -g -I. cpp/techniques/memory_layout/01_bump_allocator.cpp -o build/bin/cpp_techniques_memory_layout_01_bump_allocator && build/bin/cpp_techniques_memory_layout_01_bump_allocator
```

```sh
g++ -std=c++17 -Wall -Wextra -g -I. cpp/techniques/memory_layout/02_small_vector.cpp -o build/bin/cpp_techniques_memory_layout_02_small_vector && build/bin/cpp_techniques_memory_layout_02_small_vector
```

```sh
g++ -std=c++17 -Wall -Wextra -g -I. cpp/techniques/memory_layout/03_pointer_int_pair.cpp -o build/bin/cpp_techniques_memory_layout_03_pointer_int_pair && build/bin/cpp_techniques_memory_layout_03_pointer_int_pair
```

```sh
g++ -std=c++17 -Wall -Wextra -g -I. cpp/techniques/memory_layout/04_trailing_objects.cpp -o build/bin/cpp_techniques_memory_layout_04_trailing_objects && build/bin/cpp_techniques_memory_layout_04_trailing_objects
```

```sh
g++ -std=c++17 -Wall -Wextra -g -I. cpp/techniques/uniquing/01_string_interning.cpp -o build/bin/cpp_techniques_uniquing_01_string_interning && build/bin/cpp_techniques_uniquing_01_string_interning
```

```sh
g++ -std=c++17 -Wall -Wextra -g -I. cpp/techniques/uniquing/02_hash_consing.cpp -o build/bin/cpp_techniques_uniquing_02_hash_consing && build/bin/cpp_techniques_uniquing_02_hash_consing
```

```sh
g++ -std=c++17 -Wall -Wextra -g -I. cpp/techniques/views_and_callables/01_array_ref_string_ref.cpp -o build/bin/cpp_techniques_views_and_callables_01_array_ref_string_ref && build/bin/cpp_techniques_views_and_callables_01_array_ref_string_ref
```

```sh
g++ -std=c++17 -Wall -Wextra -g -I. cpp/techniques/views_and_callables/02_function_ref.cpp -o build/bin/cpp_techniques_views_and_callables_02_function_ref && build/bin/cpp_techniques_views_and_callables_02_function_ref
```

```sh
g++ -std=c++17 -Wall -Wextra -g -I. compiler/ir_and_ssa/04_ir_data_structures.cpp -o build/bin/ir_and_ssa_04_ir_data_structures && build/bin/ir_and_ssa_04_ir_data_structures
```

```sh
g++ -std=c++17 -Wall -Wextra -g -I. compiler/ir_and_ssa/05_ssa_construction.cpp -o build/bin/ir_and_ssa_05_ssa_construction && build/bin/ir_and_ssa_05_ssa_construction
```

## Sanitizers (useful for 04_ir_data_structures)

```sh
g++ -std=c++17 -g -fsanitize=address,undefined -I. compiler/ir_and_ssa/04_ir_data_structures.cpp -o build/bin/asan && build/bin/asan
```
