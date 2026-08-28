cmake_minimum_required(VERSION 3.20)

project(corsair_nvidia_fan_control C RC)

add_executable(corsair_nvidia_fan_control WIN32
    src/main.c
    src/ui.c
    src/corsair_hid.c
    src/nvidia_temp.c
    src/resources.rc
)

add_executable(corsair_nvidia_probe
    src/cli_probe.c
    src/corsair_hid.c
    src/nvidia_temp.c
)

target_compile_features(corsair_nvidia_fan_control PRIVATE c_std_11)
target_compile_features(corsair_nvidia_probe PRIVATE c_std_11)

target_compile_definitions(corsair_nvidia_fan_control PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)
target_compile_definitions(corsair_nvidia_probe PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

set(CORSAIR_TARGETS corsair_nvidia_fan_control corsair_nvidia_probe)
set(CORSAIR_RELEASE_CONFIG "$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>")

foreach(target_name IN LISTS CORSAIR_TARGETS)
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(${target_name} PRIVATE
            /W4
            $<${CORSAIR_RELEASE_CONFIG}:
                /O2
                /Ob3
                /Oi
                /Ot
                /Gy
                /Gw
                /clang:-march=znver4
                /clang:-mtune=znver4
                /clang:-flto=thin
            >
        )
        target_link_options(${target_name} PRIVATE
            $<${CORSAIR_RELEASE_CONFIG}:
                /OPT:REF
                /OPT:ICF
                /INCREMENTAL:NO
            >
        )
    elseif(MSVC)
        target_compile_options(${target_name} PRIVATE
            /W4
            $<${CORSAIR_RELEASE_CONFIG}:
                /O2
                /Ob3
                /Oi
                /Ot
                /Gy
                /Gw
            >
        )
        target_link_options(${target_name} PRIVATE
            $<${CORSAIR_RELEASE_CONFIG}:
                /OPT:REF
                /OPT:ICF
                /INCREMENTAL:NO
            >
        )
    else()
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            $<${CORSAIR_RELEASE_CONFIG}:
                -O3
                -march=znver4
                -mtune=znver4
                -flto=thin
                -ffunction-sections
                -fdata-sections
            >
        )
        target_link_options(${target_name} PRIVATE
            $<${CORSAIR_RELEASE_CONFIG}:
                -flto=thin
                -Wl,--gc-sections
            >
        )
    endif()
endforeach()

target_link_libraries(corsair_nvidia_fan_control PRIVATE
    advapi32
    comctl32
    hid
    shell32
    setupapi
)

target_link_libraries(corsair_nvidia_probe PRIVATE
    hid
    setupapi
)
