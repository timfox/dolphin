add_library(gxbake
  AssetBaker.cpp
  AssetBaker.h
  Cmpr.cpp
  Cmpr.h
  Config.cpp
  Config.h
  Encode.cpp
  Encode.h
  GoldSrc.cpp
  GoldSrc.h
  Md5.cpp
  Md5.h
  Process.cpp
  Process.h
  Resample.cpp
  Resample.h
)

target_link_libraries(gxbake
PUBLIC
  common

PRIVATE
  fmt::fmt
)

# The encoders reproduce numpy's float32 rounding step by step to stay byte-identical with the
# Python tool; fused multiply-adds would change the results.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(Cmpr.cpp Encode.cpp GoldSrc.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()

if(MSVC)
  # Add precompiled header
  target_link_libraries(gxbake PRIVATE use_pch)
endif()
