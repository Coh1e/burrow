# Third-party source dependencies

The native build verifies the SHA-256 of each source archive before extracting it into `.local/deps/sources/`. The source manifest records the download locations and hashes. [Pinned archives](scripts/blueprint-sources.txt)

| Dependency | Used by | Upstream license included in the Windows candidate |
| --- | --- | --- |
| [QuickJS 2026-06-04](https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz) | Blueprint JavaScript evaluation | `licenses/quickjs.txt` (MIT) |
| [libgit2 1.9.7](https://github.com/libgit2/libgit2/releases/tag/v1.9.7) | Managed Git worktrees | `licenses/libgit2.txt` (GPLv2 with linking exception and bundled component notices) |
| RFC 6234 SHA-256 implementation in the pinned libgit2 sources | Blueprint bundle verification | `licenses/rfc6234.txt` (upstream `src/util/hash/rfc6234/sha.h`) |
| [libarchive 3.7.7](https://github.com/libarchive/libarchive/releases/tag/v3.7.7) | Archive extraction | `licenses/libarchive.txt` (upstream COPYING) |
| [zlib 1.3.1](https://github.com/madler/zlib/releases/tag/v1.3.1) | Compression for archive and Git operations | `licenses/zlib.txt` (zlib license) |
| MinGW-w64 runtime from w64devkit | Windows executable | `licenses/mingw-w64-runtime.txt` (toolchain notice) |

The candidate includes Burrow's MIT license at the root of the ZIP. Its build records the files and SHA-256 values for each notice. [Project license](LICENSE) · [Windows delivery check](tests/release-acceptance/windows_delivery.py)
