cmake_minimum_required(VERSION 3.24)

project(nanokvm_rdp LANGUAGES C)

option(NANOKVM_RDP_BUILD_SERVER "Build the gateway RDP server" ON)
option(NANOKVM_RDP_BUILD_AGENT "Build the NanoKVM capture and HID agent" ON)
option(NANOKVM_RDP_BUILD_TESTS "Build local unit tests" ON)

find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NANOKVM_RDP_BUILD_TESTS)
  enable_testing()
  add_executable(nanokvm-frame-flow-test tests/frame_flow_test.c)
  target_include_directories(nanokvm-frame-flow-test PRIVATE cmd/nanokvm-rdp src)
  target_compile_options(nanokvm-frame-flow-test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME nanokvm-frame-flow-test COMMAND nanokvm-frame-flow-test)
  add_executable(nanokvm-rdp-unit
    tests/test_main.c
    src/hid.c
    src/h264.c
    src/protocol.c
    src/rtp_h264.c
    src/musl_compat.c
  )
  target_include_directories(nanokvm-rdp-unit PRIVATE src)
  target_compile_options(nanokvm-rdp-unit PRIVATE -Wall -Wextra -Werror)
  add_test(NAME nanokvm-rdp-unit COMMAND nanokvm-rdp-unit)

  add_executable(nanokvm-input-reliability-test tests/input_reliability_test.c src/hid.c src/protocol.c)
  target_include_directories(nanokvm-input-reliability-test PRIVATE src)
  target_compile_options(nanokvm-input-reliability-test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME nanokvm-input-reliability-test COMMAND nanokvm-input-reliability-test)
  set_tests_properties(nanokvm-input-reliability-test PROPERTIES TIMEOUT 10)

  add_executable(nanokvm-hid-keepalive-test tests/hid_keepalive_test.c src/hid.c)
  target_include_directories(nanokvm-hid-keepalive-test PRIVATE src)
  target_compile_options(nanokvm-hid-keepalive-test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME nanokvm-hid-keepalive-test COMMAND nanokvm-hid-keepalive-test)
  set_tests_properties(nanokvm-hid-keepalive-test PROPERTIES TIMEOUT 10)

  add_executable(nanokvm-rtp-loopback-test
    tests/rtp_loopback_test.c
    src/rtp_h264.c
  )
  target_include_directories(nanokvm-rtp-loopback-test PRIVATE src)
  target_compile_options(nanokvm-rtp-loopback-test PRIVATE -Wall -Wextra -Werror)
  add_test(NAME nanokvm-rtp-loopback-test COMMAND nanokvm-rtp-loopback-test)

  add_executable(nanokvm-rtp-client-test tests/rtp_client_test.c src/rtp_client.c src/rtp_h264.c)
  target_include_directories(nanokvm-rtp-client-test PRIVATE src)
  target_compile_options(nanokvm-rtp-client-test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME nanokvm-rtp-client-test COMMAND nanokvm-rtp-client-test)
  set_tests_properties(nanokvm-rtp-client-test PROPERTIES TIMEOUT 10)

  add_executable(nanokvm-bitmap-diff-test tests/bitmap_diff_test.c)
  target_include_directories(nanokvm-bitmap-diff-test PRIVATE cmd/nanokvm-rdp src)
  target_compile_options(nanokvm-bitmap-diff-test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME nanokvm-bitmap-diff-test COMMAND nanokvm-bitmap-diff-test)
endif()

if(NANOKVM_RDP_BUILD_AGENT)
  add_executable(nanokvm-agent
    src/agent.c
    src/hid.c
    src/h264.c
    src/protocol.c
    src/rtp_h264.c
    src/musl_compat.c
  )
  target_include_directories(nanokvm-agent PRIVATE src)
  target_compile_options(nanokvm-agent PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(nanokvm-agent PRIVATE dl Threads::Threads)
endif()

if(NANOKVM_RDP_BUILD_SERVER)
  find_program(NANOKVM_RDP_GO_EXECUTABLE go REQUIRED)
  option(NANOKVM_RDP_USE_INSTALLED_FREERDP
         "Use an installed FreeRDP package for the Go gateway" ON)
  if(NOT NANOKVM_RDP_USE_INSTALLED_FREERDP)
    message(FATAL_ERROR
      "The Go gateway requires an installed FreeRDP pkg-config export; configure with "
      "-DNANOKVM_RDP_USE_INSTALLED_FREERDP=ON -DCMAKE_PREFIX_PATH=/path/to/freerdp")
  endif()
  find_package(FreeRDP 3 REQUIRED)
  find_package(FreeRDP-Server 3 REQUIRED)

  set(NANOKVM_RDP_GO_PKG_CONFIG_PATH "" CACHE STRING
      "PKG_CONFIG_PATH used while building the cgo FreeRDP shim")
  if(NOT NANOKVM_RDP_GO_PKG_CONFIG_PATH AND CMAKE_PREFIX_PATH)
    set(NANOKVM_RDP_GO_PKG_CONFIG_DIRS)
    foreach(NANOKVM_RDP_PREFIX IN LISTS CMAKE_PREFIX_PATH)
      list(APPEND NANOKVM_RDP_GO_PKG_CONFIG_DIRS
        "${NANOKVM_RDP_PREFIX}/lib/pkgconfig"
        "${NANOKVM_RDP_PREFIX}/lib64/pkgconfig")
    endforeach()
    string(REPLACE ";" ":" NANOKVM_RDP_GO_PKG_CONFIG_PATH
      "${NANOKVM_RDP_GO_PKG_CONFIG_DIRS}")
  endif()
  set(NANOKVM_RDP_GATEWAY_BINARY "${CMAKE_CURRENT_BINARY_DIR}/nanokvm-rdp-gateway")
  add_custom_target(nanokvm-rdp-gateway ALL
    COMMAND ${CMAKE_COMMAND} -E env
      "CGO_ENABLED=1"
      "PKG_CONFIG_PATH=${NANOKVM_RDP_GO_PKG_CONFIG_PATH}"
      "${NANOKVM_RDP_GO_EXECUTABLE}" build -a -trimpath -o "${NANOKVM_RDP_GATEWAY_BINARY}"
      ./cmd/nanokvm-rdp
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    DEPENDS
      go.mod
      cmd/nanokvm-rdp/main.go
      cmd/nanokvm-rdp/bridge.go
      cmd/nanokvm-rdp/decoder.go
      cmd/nanokvm-rdp/gateway.go
      cmd/nanokvm-rdp/protocol.go
      cmd/nanokvm-rdp/rtp.go
      cmd/nanokvm-rdp/rdp_shim.c
      cmd/nanokvm-rdp/rdp_shim.h
      cmd/nanokvm-rdp/bitmap_diff.h
      cmd/nanokvm-rdp/frame_flow.h
    VERBATIM
  )

  add_executable(nanokvm-rdp-vdec-probe
    tools/vdec_probe.c
  )
  target_compile_options(nanokvm-rdp-vdec-probe PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(nanokvm-rdp-vdec-probe PRIVATE dl)

endif()
