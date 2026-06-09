# Unit test suite. Each test links molterm_core (the same objects the app
# ships) so we never hand-maintain per-test object-file lists. Run with:
#   ctest --test-dir build --output-on-failure

# Self-contained, deterministic tests — registered with CTest and run in CI.
set(MOLTERM_UNIT_TESTS
    test_geometry
    test_selection
    test_constraint
    test_register_expr
)
foreach(t ${MOLTERM_UNIT_TESTS})
    add_executable(${t} ${t}.cpp)
    target_link_libraries(${t} PRIVATE molterm_core)
    add_test(NAME ${t} COMMAND ${t})
    # Label so `ctest -L molterm` selects only our tests, excluding the
    # spurious `cpptest` gemmi injects into the build tree.
    set_tests_properties(${t} PROPERTIES LABELS molterm)
endforeach()

# Validation harnesses that compare against external mkdssp ground truth.
# They require <structure> <reference.dssp> arguments and data that is not
# committed, so they are built (to prevent bit-rot) but only registered as
# CTest cases when a reference file is provided via -DMOLTERM_DSSP_FIXTURE=
# and -DMOLTERM_DSSP_REF=.
foreach(t test_dssp test_sasa)
    add_executable(${t} ${t}.cpp)
    target_link_libraries(${t} PRIVATE molterm_core)
endforeach()
# Script-runner integration test — drives control flow through the real binary.
add_test(NAME test_script
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_script.sh $<TARGET_FILE:molterm>)
set_tests_properties(test_script PROPERTIES LABELS molterm)

if(MOLTERM_DSSP_FIXTURE AND MOLTERM_DSSP_REF)
    add_test(NAME test_dssp COMMAND test_dssp ${MOLTERM_DSSP_FIXTURE} ${MOLTERM_DSSP_REF})
    add_test(NAME test_sasa COMMAND test_sasa ${MOLTERM_DSSP_FIXTURE} ${MOLTERM_DSSP_REF})
    set_tests_properties(test_dssp test_sasa PROPERTIES LABELS molterm)
endif()
