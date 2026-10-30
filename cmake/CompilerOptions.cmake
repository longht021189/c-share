# Shared compile settings applied by every module's CMakeLists.txt so options
# stay consistent without each module repeating them.
function(c_share_set_compile_options target)
    target_compile_features(${target} PUBLIC cxx_std_17)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # Clang's source-based coverage (C_SHARE_COVERAGE, see
    # scripts/coverage.sh). Executables link the profile runtime; static
    # libraries ignore the link option.
    if(C_SHARE_COVERAGE)
        target_compile_options(${target} PRIVATE -fprofile-instr-generate -fcoverage-mapping)
        target_link_options(${target} PRIVATE -fprofile-instr-generate)
    endif()

    # Sanitizers (C_SHARE_SANITIZE, e.g. "address,undefined" or "thread"):
    # every finding stops the program, so a test with one fails.
    if(C_SHARE_SANITIZE)
        target_compile_options(${target} PRIVATE
            -fsanitize=${C_SHARE_SANITIZE} -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=${C_SHARE_SANITIZE})
    endif()
endfunction()
