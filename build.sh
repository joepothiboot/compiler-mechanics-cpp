# CMake
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure

# Or one file at a time (no CMake needed)
g++ -std=c++17 -Wall -Wextra -g -I. ir_and_ssa/05_ssa_construction.cpp -o ssa && ./ssa

# Worth doing for file 04, which does manual pointer surgery:
g++ -std=c++17 -g -fsanitize=address,undefined -I. ir_and_ssa/04_ir_data_structures.cpp -o ir && ./ir
