# Test suite.
#
# The suite is organised by proof obligation, not by source file: each executable owns one
# class of obligation and says so in its header comment.
#
# No test is given a timeout. CTest timeouts, timeout wrappers, and watchdog processes are
# prohibited in this repository: a test that does not finish is a defect to diagnose, not
# to bound.

# The probe is a real program the multiprocess and crash tests start as an independent
# operating-system process.
# The probe shares the scripted progression with the tests, so it compiles those sources
# directly instead of linking the test support library (which depends on the probe).
add_executable(bsm_probe
  support/probe.cpp
  support/scenario.cpp
  support/fixture.cpp)
target_include_directories(bsm_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(bsm_probe PRIVATE black_start_manager)
black_start_manager_apply_strict_options(bsm_probe)

add_library(bsm_test_support STATIC
  support/test_harness.cpp
  support/fixture.cpp
  support/scenario.cpp
  support/proc.cpp)
target_include_directories(bsm_test_support PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(bsm_test_support PUBLIC black_start_manager)
target_compile_definitions(bsm_test_support PUBLIC
  "BSM_PROBE_EXECUTABLE=\"$<TARGET_FILE:bsm_probe>\"")
add_dependencies(bsm_test_support bsm_probe)
black_start_manager_apply_strict_options(bsm_test_support)

if(TARGET bsm_cli)
  target_compile_definitions(bsm_test_support PUBLIC
    "BSM_CLI_EXECUTABLE=\"$<TARGET_FILE:bsm_cli>\""
    "BSM_PLANT_EXECUTABLE=\"$<TARGET_FILE:bsm_plant>\"")
  add_dependencies(bsm_test_support bsm_cli bsm_plant)
endif()

function(black_start_manager_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE bsm_test_support)
  # Adversarial artifacts are produced with the same encoders and digests the library
  # uses, so the tests need the internal headers.
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}")
  black_start_manager_apply_strict_options(${name})
  add_test(NAME ${name} COMMAND ${name})
  # Test state is written under the build tree, never into the source tree.
  set_tests_properties(${name} PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

# Proof obligations: digests, canonical encoding, identities, checked arithmetic.
black_start_manager_add_test(bsm_test_primitives test_primitives.cpp)
# Proof obligations: plan compilation, deterministic order, refusal of contradictions.
black_start_manager_add_test(bsm_test_plan test_plan.cpp)
# Proof obligations: evidence freshness, expectations, recovery demotion, contradictions.
black_start_manager_add_test(bsm_test_evidence test_evidence.cpp)
# Proof obligations: session lifecycle, gates, holds, fencing, replan, return to service.
black_start_manager_add_test(bsm_test_session test_session.cpp)
# Proof obligations: durable attempt identity, idempotent replay, lost replies, budgets.
black_start_manager_add_test(bsm_test_attempts test_attempts.cpp)
# Proof obligations: durable publication, integrity, recovery, corruption refusal.
black_start_manager_add_test(bsm_test_persistence test_persistence.cpp)
# Proof obligations: seeded randomized state-machine invariants and a reference model.
black_start_manager_add_test(bsm_test_property test_property.cpp)
# Proof obligations: malformed, contradictory, replayed, and boundary-shaped input.
black_start_manager_add_test(bsm_test_adversarial test_adversarial.cpp)
# Proof obligations: real independent processes, writer death, crash injection, recovery.
black_start_manager_add_test(bsm_test_multiprocess test_multiprocess.cpp)

if(TARGET bsm_cli AND TARGET bsm_plant)
  black_start_manager_add_test(bsm_test_cli test_cli.cpp)
endif()

# The downstream consumer is configured, built, and run out of tree against an installed
# package. It is enabled by pointing BLACK_START_MANAGER_DOWNSTREAM_PREFIX at an install
# prefix produced by this project.
if(BLACK_START_MANAGER_DOWNSTREAM_PREFIX)
  add_test(NAME bsm_test_downstream_install
    COMMAND "${CMAKE_COMMAND}"
      -DBSM_SOURCE_DIR=${CMAKE_SOURCE_DIR}
      -DBSM_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/downstream
      -DBSM_PREFIX=${BLACK_START_MANAGER_DOWNSTREAM_PREFIX}
      -DBSM_GENERATOR=${CMAKE_GENERATOR}
      -DBSM_CONFIG=$<CONFIG>
      -P "${CMAKE_SOURCE_DIR}/cmake/RunDownstreamCheck.cmake")
  set_tests_properties(bsm_test_downstream_install PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
