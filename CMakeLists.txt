if(CONFIG_ZMK_HOST_STATUS)
  # Build ID shown by host apps: CI commit if available, else local git.
  set(HOST_STATUS_BUILD_ID "local")
  if(DEFINED ENV{GITHUB_SHA})
    string(SUBSTRING "$ENV{GITHUB_SHA}" 0 7 HOST_STATUS_BUILD_ID)
  elseif(DEFINED ZMK_CONFIG)
    execute_process(
      COMMAND git -C "${ZMK_CONFIG}" rev-parse --short=7 HEAD
      OUTPUT_VARIABLE _hs_sha OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _hs_res ERROR_QUIET)
    if(_hs_res EQUAL 0 AND NOT "${_hs_sha}" STREQUAL "")
      set(HOST_STATUS_BUILD_ID "${_hs_sha}")
    endif()
  endif()
  message(STATUS "zmk-host-status build id: ${HOST_STATUS_BUILD_ID}")
  target_sources(app PRIVATE src/host_status.c)
  target_compile_definitions(app PRIVATE HOST_STATUS_BUILD_ID="${HOST_STATUS_BUILD_ID}")
endif()
