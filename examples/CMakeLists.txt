# Examples. Each program is a complete, runnable walkthrough of one behaviour, written
# against the public API only.

add_executable(example_lifecycle example_lifecycle.cpp)
target_link_libraries(example_lifecycle PRIVATE black_start_manager)
black_start_manager_apply_strict_options(example_lifecycle)

add_executable(example_stale_authority example_stale_authority.cpp)
target_link_libraries(example_stale_authority PRIVATE black_start_manager)
black_start_manager_apply_strict_options(example_stale_authority)

add_executable(example_crash_recovery example_crash_recovery.cpp)
target_link_libraries(example_crash_recovery PRIVATE black_start_manager)
black_start_manager_apply_strict_options(example_crash_recovery)

add_custom_target(run_examples
  COMMAND example_lifecycle "${CMAKE_CURRENT_BINARY_DIR}/example-store"
  COMMAND example_stale_authority "${CMAKE_CURRENT_BINARY_DIR}/example-stale-store"
  COMMAND example_crash_recovery "${CMAKE_CURRENT_BINARY_DIR}/example-crash-store"
  DEPENDS example_lifecycle example_stale_authority example_crash_recovery
  COMMENT "Running the Black Start Manager examples"
  VERBATIM)
