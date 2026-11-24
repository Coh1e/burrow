# 旧 Linux 最小 brw

在 Linux x86-64 上预置 C 编译器、静态 libc 开发文件、Make 和 CMake。将 [fetch-deps.ps1](../scripts/fetch-deps.ps1) 固定的 libarchive 3.7.7 和 zlib 1.3.1 源码归档放在仓库的 `.local/deps/sources/`，校验 SHA-256 后在该目录解压。从仓库根目录执行：

```sh
mkdir build-minimal && cd build-minimal
CC=gcc-4.8 cmake -DBRW_MINIMAL=ON -DCMAKE_BUILD_TYPE=MinSizeRel ../brw
cmake --build .
ctest --output-on-failure
```

- `brw search FILE TEXT` 按字面量查找非重叠匹配，每行打印一个从零开始的 UTF-8 字节偏移。没有命中时返回失败。
- `brw replace FILE FROM TO NEWFILE [COUNT]` 写入新文件。指定次数时须完全匹配，零次也可作为前提；省略时须至少命中一次。搜索与替换读取整个输入，拒绝空搜索串和内含 NUL 的文件。
- `brw pack ARCHIVE FILE...` 将一个或多个普通文件按基本文件名写入新的 gzip 压缩 tar，拒绝重名。
- `brw unpack ARCHIVE DIRECTORY` 将普通或 gzip 压缩的 tar 解到已有目录，支持普通文件和子目录。绝对路径、父目录分量、链接条目和特殊文件会报错。解包保留已有文件。失败时，已完成的文件和已建目录保留，未写完的文件不会发布。

在已经隔离网络且挂载了 proc 的旧 Linux 用户空间内执行 `sh scripts/check-brw-legacy.sh`。脚本从仓库内的归档核验摘要，在全新目录配置、构建并运行 CTest，检查编译工具、网络接口、ELF 解释器、动态依赖和 2 MiB 体积上限。
