# Benchmarks measure completed useful operations, including the durability cost each
# operation actually pays. Workload scale and REAL/SYNTHETIC provenance are stated in the
# output.

add_executable(bench_durability bench_durability.cpp)
target_link_libraries(bench_durability PRIVATE black_start_manager)
black_start_manager_apply_strict_options(bench_durability)
