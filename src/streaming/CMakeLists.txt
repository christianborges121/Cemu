# Isolated CemuPad streaming subsystem (Phase 4.0).
# New discovery, pairing and bridge code lives here so core Cemu files only
# ever include streaming/*.h and resolve symbols at the final CemuBin link.
# Media servers (VideoStreamServer/VideoEncoder/StreamingCapture) move here in
# the Step 3.1 follow-up once the move is verified with a full Release build.
add_library(CemuStreaming STATIC
  CemuPadBridge.cpp
  CemuPadBridge.h
  DiscoveryServer.cpp
  DiscoveryServer.h
  ReedSolomon.cpp
  ReedSolomon.h
)

# PUBLIC "../" so consumers can include "streaming/CemuPadBridge.h"
# (same convention as CemuCafe/CemuInput/CemuWxGui).
target_include_directories(CemuStreaming PUBLIC "../")

target_link_libraries(CemuStreaming PUBLIC
  CemuCommon
)

target_link_libraries(CemuStreaming PRIVATE
  CemuInput
  CemuComponents
)

if(WIN32)
  target_link_libraries(CemuStreaming PRIVATE ws2_32)
endif()

set_property(TARGET CemuStreaming PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

cemu_use_precompiled_header(CemuStreaming)
