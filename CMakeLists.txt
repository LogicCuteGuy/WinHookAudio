cmake_minimum_required(VERSION 3.24)
project(WinHookAudio VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "WinHookAudio currently requires Windows, MSVC, and x64.")
endif()

add_executable(winhookaudio-probe
    src/probe/main.cpp
    src/probe/mmdevice.cpp
    src/probe/ks.cpp
    src/probe/asio_registry.cpp)
target_compile_features(winhookaudio-probe PRIVATE cxx_std_20)
set_property(TARGET winhookaudio-probe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_definitions(winhookaudio-probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(winhookaudio-probe PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
target_link_libraries(winhookaudio-probe PRIVATE ole32 propsys setupapi ksuser advapi32 uuid)
install(TARGETS winhookaudio-probe RUNTIME DESTINATION bin)
install(FILES docs/probe-vm.md DESTINATION . RENAME README.md)
install(FILES scripts/Run-Probe.ps1 DESTINATION .)

include(CTest)
if(BUILD_TESTING)
    find_program(POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)
    add_test(NAME probe-offline-cli COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/Probe-Cli.ps1" -Probe "$<TARGET_FILE:winhookaudio-probe>")
    set_tests_properties(probe-offline-cli PROPERTIES TIMEOUT 30)
endif()

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_FILE_NAME "WinHookAudio-Probe-${PROJECT_VERSION}-windows-x64")
include(CPack)
