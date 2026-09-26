# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "")
  file(REMOVE_RECURSE
  "CMakeFiles\\agent-monitor_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\agent-monitor_autogen.dir\\ParseCache.txt"
  "agent-monitor_autogen"
  )
endif()
