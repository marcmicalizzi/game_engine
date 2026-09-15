# io (foundation)

**Purpose.** File access for the engine and its tools (docs/plan/02-architecture.md §2.3): status-returning native file operations in UTF-8, a mount table so engine code addresses files as `content://levels/a.json` or `cache://ddc/...` without knowing where a project keeps them, and asynchronous reads on the job system's Efficiency pool. The content-addressed derived-data store (domain/ddc) and streaming build on this.

**Owned data.** The `Vfs` mount table. Nothing else; files belong to the OS.

**Invariants (tested).**
- Every operation returns a `Status`; nothing throws. `std::filesystem` is used through its `error_code` overloads only.
- `write_file_atomic` writes a sibling temporary and renames over the target, so a reader sees the old or the new file, never a mix, and no temporary is left behind on failure.
- `Vfs::resolve` never yields a path outside the mount root: absolute paths, `..` that escapes, and drive prefixes are `InvalidPath`; unknown schemes are `UnknownMount`; writes through read-only mounts are `ReadOnly`. Paths without `scheme://` pass through as native paths, normalized.
- `normalize_path` uses forward slashes on every platform and is a pure string operation.
- `list_directory` returns entries sorted by name, so tools and agents see stable order.

**Public API.** `foundation/io/vfs.h`: `Status`, `status_name`, `FileInfo`, `DirEntry`; path helpers `normalize_path`, `join_path`, `parent_path`, `file_name`, `extension`, `is_absolute_path`; native operations `read_file`, `write_file`, `write_file_atomic`, `stat_file`, `exists`, `list_directory`, `make_directories`, `remove_file`, `remove_directory_recursive`, `rename_path`; `Mount`, `Vfs` (`mount`, `unmount`, `resolve`, `read`, `write`, `stat`, `exists`, `list`, `make_directories`, `remove`); `AsyncRead`, `read_async`, `wait_all`.

**Depends on.** `base`, `containers`, `jobs`, `time`.

**Testing.** `tools/dev.ps1 test -Filter io`. Works in a temporary directory: path normalization, the native operation set including atomic replacement and cleanup, mount resolution and escape refusal, and asynchronous reads with a two-worker job system.

**Performance notes.** Reads go through a 64 KB stdio buffer with the whole size reserved up front. Asynchronous reads are synchronous reads on Efficiency workers, which is the right shape for tooling and for the first streaming implementation; OS asynchronous I/O (overlapped or io_uring) and DirectStorage-style GPU decompression slot in behind the same `AsyncRead` request when streaming bandwidth asks for them.
