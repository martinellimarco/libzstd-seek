add_executable(tar-zst-list tar-zst-list.c)
target_link_libraries(tar-zst-list zstd m zstd-seek)