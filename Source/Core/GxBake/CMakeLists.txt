add_library(gxbake
  AssetBaker.cpp
  AssetBaker.h
  Bsp30.cpp
  Bsp30.h
  Cmpr.cpp
  Cmpr.h
  Config.cpp
  Config.h
  DataTable.cpp
  DataTable.h
  Encode.cpp
  Encode.h
  GoldSrc.cpp
  GoldSrc.h
  Md5.cpp
  Md5.h
  PilQuant.cpp
  PilQuant.h
  Process.cpp
  Process.h
  Quake.cpp
  Quake.h
  Resample.cpp
  Resample.h
  Source.h
  SourceMap.cpp
  SourceModel.cpp
  Studio.cpp
  Studio.h
  Vtf.cpp
  Vtf.h
)

target_link_libraries(gxbake
PUBLIC
  common

PRIVATE
  fmt::fmt
  LibLZMA::LibLZMA
  MINIZIP::minizip-ng
)

# The encoders reproduce numpy's float32 rounding step by step to stay byte-identical with the
# Python tool; fused multiply-adds would change the results.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(Bsp30.cpp Cmpr.cpp Encode.cpp GoldSrc.cpp Quake.cpp SourceMap.cpp SourceModel.cpp Studio.cpp Vtf.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()

if(MSVC)
  # Add precompiled header
  target_link_libraries(gxbake PRIVATE use_pch)
endif()
