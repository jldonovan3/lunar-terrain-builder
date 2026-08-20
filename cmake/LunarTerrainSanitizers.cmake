function(lunar_terrain_enable_sanitizers target)
    if(NOT LUNAR_TERRAIN_ENABLE_SANITIZERS)
        return()
    endif()

    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "LUNAR_TERRAIN_ENABLE_SANITIZERS is supported only on Linux")
    endif()

    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "The sanitizer workflow requires GCC or Clang")
    endif()

    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
endfunction()

