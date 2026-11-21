# SPDX-License-Identifier: MIT
# Vendored zlib 1.3.1 (UPSTREAM), built as a static library for layer DLLs and samples.
add_library(xsl_zlib STATIC adler32.c compress.c crc32.c deflate.c infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c)
target_include_directories(xsl_zlib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(xsl_zlib PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(xsl_zlib PRIVATE -Wno-everything)
