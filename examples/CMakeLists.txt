add_executable(tar-zst-list tar-zst-list.c)
target_link_libraries(tar-zst-list zstd m zstd-seek)

add_executable(decompressor decompressor.c)
target_link_libraries(decompressor m zstd-seek)