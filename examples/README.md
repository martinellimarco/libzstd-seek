# Examples

- **tar-zst-list**: An example program that takes a .tar.zst archive in input and list all the files inside. Each time a tar header is decoded it calculate the size and seek to the next file.