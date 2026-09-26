cmake_minimum_required(VERSION 3.24)

project(nanokvm_rdp LANGUAGES C)

option(NANOKVM_RDP_BUILD_SERVER "Build the gateway RDP server" ON)
option(NANOKVM_RDP_BUILD_AGENT "Build the NanoKVM capture and HID agent" ON)
option(NANOKVM_RDP_BUILD_TESTS "Build local unit tests" ON)
option(NANOKVM_RDP_TEST_FFMPEG "Test live decoder latency (requires ffmpeg with libx264 on PATH)" OFF)

find_package(Threads REQUIRED)
if(NANOKVM_RDP_BUILD_SERVER OR NANOKVM_RDP_TEST_FFMPEG)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NANOKVM_RDP_BUILD_TESTS)
  enable_testing()
  if(NANOKVM_RDP_TEST_FFMPEG)
    add_executable(nanokvm-ffmpeg-latency-test tests/ffmpeg_latency_test.c src/ffmpeg_decoder.c)
    target_include_directories(nanokvm-ffmpeg-latency-test PRIVATE src)
    target_compile_options(nanokvm-ffmpeg-latency-test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    target_link_libraries(nanokvm-ffmpeg-latency-test PRIVATE Threads::Threads PkgConfig::FFMPEG)
    add_test(NAME nanokvm-ffmpeg-latency-test COMMAND nanokvm-ffmpeg-latency-test)
    set_tests_properties(nanokvm-ffmpeg-latency-test PROPERTIES TIMEOUT 10)
  endif()
  add_executable(nanokvm-frame-flow-test tests/frame_flow_test.c)
  target_include_directories(nanokvm-frame-flow-test PRIVATE src)
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
  target_include_directories(nanokvm-bitmap-diff-test PRIVATE src)
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
  option(NANOKVM_RDP_USE_INSTALLED_FREERDP
         "Use an installed FreeRDP package instead of embedding its source tree" OFF)
  set(NANOKVM_RDP_FREERDP_DIR "" CACHE PATH
      "Optional FreeRDP source tree; omit when using an installed FreeRDP development package")
  set(NANOKVM_RDP_OPENSSL_ROOT "" CACHE PATH
      "Cross-compiled OpenSSL prefix (required for a cross build)")

  if(NANOKVM_RDP_USE_INSTALLED_FREERDP)
    find_package(FreeRDP 3 REQUIRED)
    find_package(FreeRDP-Server 3 REQUIRED)
  else()
    if(NOT NANOKVM_RDP_FREERDP_DIR OR NOT EXISTS "${NANOKVM_RDP_FREERDP_DIR}/CMakeLists.txt")
      message(FATAL_ERROR "Set NANOKVM_RDP_FREERDP_DIR to a FreeRDP source tree (gateway only); agent builds without FreeRDP")
    endif()

    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(WITH_SERVER ON CACHE BOOL "" FORCE)
    set(WITH_CLIENT OFF CACHE BOOL "" FORCE)
    set(WITH_SAMPLE OFF CACHE BOOL "" FORCE)
    set(WITH_SHADOW OFF CACHE BOOL "" FORCE)
    set(WITH_PROXY OFF CACHE BOOL "" FORCE)
    set(WITH_PLATFORM_SERVER OFF CACHE BOOL "" FORCE)
    set(WITH_X11 OFF CACHE BOOL "" FORCE)
    set(WITH_WAYLAND OFF CACHE BOOL "" FORCE)
    set(WITH_CAIRO OFF CACHE BOOL "" FORCE)
    set(WITH_FFMPEG OFF CACHE BOOL "" FORCE)
    set(WITH_JPEG OFF CACHE BOOL "" FORCE)
    set(WITH_PNG OFF CACHE BOOL "" FORCE)
    set(WITH_ZLIB OFF CACHE BOOL "" FORCE)
    set(WITH_CUPS OFF CACHE BOOL "" FORCE)
    set(WITH_PCSC OFF CACHE BOOL "" FORCE)
    set(WITH_LIBUSB OFF CACHE BOOL "" FORCE)
    set(WITH_MANPAGES OFF CACHE BOOL "" FORCE)
    set(WITH_SMARTCARD_EMULATE OFF CACHE BOOL "" FORCE)
    set(WITH_KRB5 OFF CACHE BOOL "" FORCE)
    set(WITH_ALSA OFF CACHE BOOL "" FORCE)
    set(WITH_PULSE OFF CACHE BOOL "" FORCE)
    set(WITH_UNICODE_BUILTIN ON CACHE BOOL "" FORCE)
    set(WITH_JSON_DISABLED ON CACHE BOOL "" FORCE)
    set(WITH_URIPARSER OFF CACHE BOOL "" FORCE)
    set(WITH_FUSE OFF CACHE BOOL "" FORCE)
    set(WITH_OPUS OFF CACHE BOOL "" FORCE)
    set(WITH_AAD OFF CACHE BOOL "" FORCE)
    set(USE_UNWIND OFF CACHE BOOL "" FORCE)
    set(WITH_DSP_FFMPEG OFF CACHE BOOL "" FORCE)
    set(WITH_VIDEO_FFMPEG OFF CACHE BOOL "" FORCE)
    set(WITH_SWSCALE OFF CACHE BOOL "" FORCE)
    set(WITH_GFX_H264 ON CACHE BOOL "" FORCE)
    set(WITH_OPENSSL ON CACHE BOOL "" FORCE)
    if(NANOKVM_RDP_OPENSSL_ROOT)
      set(OPENSSL_ROOT_DIR "${NANOKVM_RDP_OPENSSL_ROOT}" CACHE PATH "" FORCE)
    endif()

    add_subdirectory("${NANOKVM_RDP_FREERDP_DIR}" "${CMAKE_BINARY_DIR}/freerdp")
  endif()

  add_executable(nanokvm-rdp-gateway
    src/main.c
    src/ffmpeg_decoder.c
    src/hid.c
    src/h264.c
    src/protocol.c
    src/rtp_h264.c
    src/rtp_client.c
  )
  target_include_directories(nanokvm-rdp-gateway PRIVATE src)
  if(NANOKVM_RDP_USE_INSTALLED_FREERDP)
    target_include_directories(nanokvm-rdp-gateway PRIVATE "${FreeRDP_INCLUDE_DIR}")
  else()
    target_include_directories(nanokvm-rdp-gateway PRIVATE
      "${NANOKVM_RDP_FREERDP_DIR}/include"
      "${NANOKVM_RDP_FREERDP_DIR}/winpr/include"
      "${CMAKE_BINARY_DIR}/freerdp/include"
      "${CMAKE_BINARY_DIR}/freerdp/winpr/include"
    )
  endif()
  target_compile_options(nanokvm-rdp-gateway PRIVATE -Wall -Wextra -Werror -Wno-deprecated-declarations)
  target_link_libraries(nanokvm-rdp-gateway PRIVATE freerdp-server freerdp winpr dl PkgConfig::FFMPEG)

  add_executable(nanokvm-rdp-vdec-probe
    tools/vdec_probe.c
  )
  target_compile_options(nanokvm-rdp-vdec-probe PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(nanokvm-rdp-vdec-probe PRIVATE dl)

endif()
