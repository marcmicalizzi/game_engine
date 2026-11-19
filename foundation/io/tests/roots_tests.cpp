// io::Roots (roadmap T174, ADR-0071; docs/subsystems/io.md, "Roots"). Every case is a way round
// the check that would land under a root, or reach out of one, if the rule it names were taken
// out: an escape that is refused for some other reason first proves nothing. All scratch is under
// a TempDir, and no case names a path of the machine it runs on.
#include <foundation/io/roots.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#if ENGINE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#endif

using namespace engine;
using namespace engine::io;
using TempDir = test::TempDir;

namespace {

std::string canon(const std::string& p) {
  std::string out;
  REQUIRE(canonical_path(p, {}, out) == Refusal::none);
  return out;
}

void make_dir(const std::string& p) { REQUIRE(make_directories(p) == Status::Ok); }
void put(const std::string& p, std::string_view data) {
  REQUIRE(write_file(p, data) == Status::Ok);
}

std::filesystem::path fs_path(const std::string& p) {
  return std::filesystem::path(
      std::u8string_view(reinterpret_cast<const char8_t*>(p.data()), p.size()));
}

// A directory symbolic link where the process may make one (Windows needs developer mode or
// elevation; the case says so and skips without it).
bool make_dir_symlink(const std::string& target, const std::string& link) {
  std::error_code ec;
  std::filesystem::create_directory_symlink(fs_path(target), fs_path(link), ec);
  return !ec;
}
bool make_file_symlink(const std::string& target, const std::string& link) {
  std::error_code ec;
  std::filesystem::create_symlink(fs_path(target), fs_path(link), ec);
  return !ec;
}

#if ENGINE_PLATFORM_WINDOWS
std::wstring wide(const std::string& s) {
  std::wstring w(static_cast<usize>(MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                                        static_cast<int>(s.size()), nullptr, 0)),
                 L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(),
                      static_cast<int>(w.size()));
  for (wchar_t& c : w)
    if (c == L'/') c = L'\\';
  return w;
}

// A junction, which any user may make: an empty directory given a mount-point reparse point.
bool make_junction(const std::string& target, const std::string& link) {
  if (!CreateDirectoryW(wide(link).c_str(), nullptr)) return false;
  const HANDLE h = CreateFileW(wide(link).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  const std::wstring substitute = L"\\??\\" + wide(target);
  const std::wstring print = wide(target);
  const usize names = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
  std::string buf(8 + 8 + names, '\0');
  auto put16 = [&](usize at, usize v) {
    buf[at] = static_cast<char>(v & 0xFF);
    buf[at + 1] = static_cast<char>((v >> 8) & 0xFF);
  };
  const u32 tag = 0xA0000003u;  // IO_REPARSE_TAG_MOUNT_POINT
  std::memcpy(buf.data(), &tag, 4);
  put16(4, 8 + names);                                   // ReparseDataLength
  put16(8, 0);                                           // SubstituteNameOffset
  put16(10, substitute.size() * sizeof(wchar_t));        // SubstituteNameLength
  put16(12, (substitute.size() + 1) * sizeof(wchar_t));  // PrintNameOffset
  put16(14, print.size() * sizeof(wchar_t));             // PrintNameLength
  std::memcpy(buf.data() + 16, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
  std::memcpy(buf.data() + 16 + (substitute.size() + 1) * sizeof(wchar_t), print.c_str(),
              (print.size() + 1) * sizeof(wchar_t));
  DWORD returned = 0;
  const BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(),
                                  static_cast<DWORD>(buf.size()), nullptr, 0, &returned, nullptr);
  CloseHandle(h);
  return ok != FALSE;
}
#endif

// A directory link of whichever kind this system lets a test make: a junction on Windows, a
// symbolic link on Linux.
bool make_dir_link(const std::string& target, const std::string& link) {
#if ENGINE_PLATFORM_WINDOWS
  return make_junction(target, link);
#else
  return make_dir_symlink(target, link);
#endif
}

struct Fixture {
  TempDir tmp;
  std::string root;     // a read-and-write root, "work"
  std::string outside;  // a directory beside it, under no root
  Roots roots;
  Fixture() {
    root = tmp.path() + "/work";
    outside = tmp.path() + "/outside";
    make_dir(root);
    make_dir(outside);
    put(outside + "/secret.txt", "secret");
    put(root + "/inside.txt", "inside");
    REQUIRE(roots.add("work", root, true) == Status::Ok);
  }
  Refusal refuse(std::string_view path, Access access = Access::read,
                 std::string_view base = {}) const {
    Admitted a;
    Refusal why = Refusal::none;
    const bool ok = roots.admit(path, base.empty() ? std::string_view(root) : base, access, a, why);
    CHECK_FALSE(ok);
    CHECK(a.native.empty());
    return why;
  }
  Admitted allow(std::string_view path, Access access = Access::read) const {
    Admitted a;
    Refusal why = Refusal::none;
    const bool ok = roots.admit(path, root, access, a, why);
    INFO("refusal: " << refusal_name(why));
    CHECK(ok);
    return a;
  }
};

}  // namespace

TEST_CASE("roots: no roots admit nothing") {
  TempDir tmp;
  Roots none;
  Admitted a;
  Refusal why = Refusal::none;
  CHECK_FALSE(none.admit("a.txt", tmp.path(), Access::read, a, why));
  CHECK(why == Refusal::outside);
  CHECK_FALSE(none.admit(tmp.path(), {}, Access::read, a, why));
  CHECK(why == Refusal::outside);
}

TEST_CASE("roots: a path under a root is admitted and answered as root and relative") {
  Fixture f;
  const Admitted a = f.allow("inside.txt");
  CHECK(a.native == canon(f.root) + "/inside.txt");
  CHECK(a.root == "work");
  CHECK(a.relative == "inside.txt");
  CHECK(a.portable() == "work://inside.txt");
  // The portable form names the same file, and is what an answer or a record carries.
  const Admitted b = f.allow("work://inside.txt");
  CHECK(b.native == a.native);
  const Admitted c = f.allow("work://sub/new.png", Access::write);
  CHECK(c.relative == "sub/new.png");
  const Admitted d = f.allow("work://");
  CHECK(d.relative.empty());
  std::string p;
  CHECK(f.roots.portable(a.native, p));
  CHECK(p == "work://inside.txt");
  CHECK_FALSE(f.roots.portable(canon(f.outside) + "/secret.txt", p));
}

TEST_CASE("roots: escapes by spelling outside a root") {
  Fixture f;
  CHECK(f.refuse(f.outside + "/secret.txt") == Refusal::outside);
  CHECK(f.refuse("../outside/secret.txt") == Refusal::outside);
  CHECK(f.refuse("a/../../outside/secret.txt") == Refusal::outside);
  // A root's name as a prefix is not the root: work vs workshop.
  make_dir(f.tmp.path() + "/workshop");
  CHECK(f.refuse(f.tmp.path() + "/workshop") == Refusal::outside);
  CHECK(f.refuse(f.root + "shop") == Refusal::outside);
  // ".." above the volume's root.
#if ENGINE_PLATFORM_WINDOWS
  CHECK(f.refuse("C:/../x") == Refusal::above_root);
#else
  CHECK(f.refuse("/../x") == Refusal::above_root);
#endif
  // A named path stays under the root it names.
  CHECK(f.refuse("work://../outside/secret.txt") == Refusal::outside);
  CHECK(f.refuse("work:///x") == Refusal::absolute_relative);
  CHECK(f.refuse("work://C:/x") == Refusal::absolute_relative);
}

TEST_CASE("roots: a root this host does not have, or whose folder is gone, is not resolved") {
  Fixture f;
  CHECK(f.refuse("nope://inside.txt") == Refusal::unknown_root);
  const std::string gone = f.tmp.path() + "/gone";
  make_dir(gone);
  REQUIRE(f.roots.add("gone", gone, false) == Status::Ok);
  REQUIRE(remove_directory_recursive(gone) == Status::Ok);
  CHECK(f.refuse("gone://x.txt") == Refusal::unknown_root);
}

TEST_CASE("roots: a relative path with no base is refused") {
  Fixture f;
  Admitted a;
  Refusal why = Refusal::none;
  CHECK_FALSE(f.roots.admit("inside.txt", {}, Access::read, a, why));
  CHECK(why == Refusal::no_base);
}

TEST_CASE("roots: the lexical rules refuse what would otherwise land under the root") {
  Fixture f;
  CHECK(f.refuse("") == Refusal::empty);
  CHECK(f.refuse(std::string("a\x01.txt")) == Refusal::control);
  CHECK(f.refuse("a\x7F") == Refusal::control);
  CHECK(f.refuse("\xFF\xFE") == Refusal::not_utf8);
  CHECK(f.refuse("\xC0\xAF") == Refusal::not_utf8);  // an overlong '/'
  CHECK(f.refuse(std::string(40000, 'a')) == Refusal::too_long);
  for (const char* p : {"a<b", "a>b", "a\"b", "a|b", "a?b", "a*b"}) {
    INFO(p);
    CHECK(f.refuse(p) == Refusal::reserved_character);
  }
  // Alternate data streams, and the stream that makes a directory.
  CHECK(f.refuse("inside.txt:hidden") == Refusal::stream);
  CHECK(f.refuse("inside.txt::$DATA") == Refusal::stream);
  CHECK(f.refuse("dir::$INDEX_ALLOCATION", Access::make_directory) == Refusal::stream);
  CHECK(f.refuse(f.root + "/inside.txt:x") == Refusal::stream);
  // Drive-relative: "x in drive C's current directory".
  CHECK(f.refuse("C:inside.txt") == Refusal::stream);
  // Namespace prefixes in any mix of separators ('?' is reserved, so \\?\ and \??\ fall there).
  CHECK(f.refuse("\\\\.\\C:\\x") == Refusal::namespace_prefix);
  CHECK(f.refuse("//./C:/x") == Refusal::namespace_prefix);
  CHECK(f.refuse("\\\\?\\" + f.root + "/inside.txt") == Refusal::reserved_character);
  CHECK(f.refuse("\\??\\C:\\x") == Refusal::reserved_character);
#if ENGINE_PLATFORM_WINDOWS
  CHECK(f.refuse("/inside.txt") == Refusal::drive_relative);
#endif
  // UNC, with no root that is one.
  CHECK(f.refuse("//server/share/x") == Refusal::unc);
  CHECK(f.refuse("\\\\server\\share\\x") == Refusal::unc);
}

TEST_CASE("roots: a root on a network share is refused at start, and no UNC path is resolved") {
  Fixture f;
  // A server under RFC 2606's .invalid, which no resolver answers: the refusal must come from
  // the spelling, before anything is made or opened, and a regression that probed it reaches
  // nobody.
  Refusal why = Refusal::none;
  CHECK(f.roots.add("share", "//engine-tests.invalid/share", false, true, &why) ==
        Status::InvalidPath);
  CHECK(why == Refusal::unc);
  why = Refusal::none;
  CHECK(f.roots.add("share_rw", "\\\\engine-tests.invalid\\share\\work", true, true, &why) ==
        Status::InvalidPath);
  CHECK(why == Refusal::unc);
  CHECK(f.roots.find("share") == nullptr);
  CHECK(f.roots.find("share_rw") == nullptr);
  // A root that is not on a share answers no refusal step.
  why = Refusal::unc;
  REQUIRE(f.roots.add("plain", f.outside, false, true, &why) == Status::Ok);
  CHECK(why == Refusal::none);
  // No root is UNC, so every UNC spelling is refused before step 8, for any access.
  CHECK(f.refuse("//engine-tests.invalid/share/x") == Refusal::unc);
  CHECK(f.refuse("//engine-tests.invalid/share/x", Access::write) == Refusal::unc);
  CHECK(f.refuse("\\\\engine-tests.invalid\\share", Access::make_directory) == Refusal::unc);
}

// A document's out/ is the one root the host derives rather than the operator spelling it, so
// following a link there handed the choice of the host's writable root to whoever made the
// document: add() follows a root that is a link, which is the rule for the operator's own.
TEST_CASE("roots: a document's out that is a link, a file or a dangling link is not a root") {
  Fixture f;
  const std::string doc = f.tmp.path() + "/doc";
  make_dir(doc);
  REQUIRE(f.roots.add("doc", doc, false) == Status::Ok);
  // Nothing there yet: a root made by its first write, canonically the document's own out/.
  {
    Roots r;
    REQUIRE(r.add("doc", doc, false) == Status::Ok);
    REQUIRE(r.add_inside("out", "doc", "out") == Status::Ok);
    REQUIRE(r.find("out") != nullptr);
    CHECK(r.find("out")->canonical == canon(doc) + "/out");
    CHECK(r.find("out")->made_on_write);
  }
  // A plain directory.
  make_dir(doc + "/out");
  {
    Roots r;
    REQUIRE(r.add("doc", doc, false) == Status::Ok);
    CHECK(r.add_inside("out", "doc", "out") == Status::Ok);
  }
  REQUIRE(remove_directory_recursive(doc + "/out") == Status::Ok);
  // A file.
  put(doc + "/out", "not a directory");
  {
    Roots r;
    REQUIRE(r.add("doc", doc, false) == Status::Ok);
    CHECK(r.add_inside("out", "doc", "out") == Status::NotDirectory);
    CHECK(r.find("out") == nullptr);
  }
  REQUIRE(remove_file(doc + "/out") == Status::Ok);
  // A link to a directory outside the document: never the root, and out:// names nothing.
  if (!make_dir_link(f.outside, doc + "/out")) {
    MESSAGE("this process may not make a directory link; the link half is skipped");
    return;
  }
  {
    Roots r;
    REQUIRE(r.add("doc", doc, false) == Status::Ok);
    CHECK(r.add_inside("out", "doc", "out") == Status::NotDirectory);
    CHECK(r.find("out") == nullptr);
    Admitted a;
    Refusal why = Refusal::none;
    CHECK_FALSE(r.admit("out://x.png", {}, Access::write, a, why));
    CHECK(why == Refusal::unknown_root);
  }
  // The same link with its target gone: before, add() refused it and the host did not start.
  const std::string target = f.tmp.path() + "/target";
  make_dir(target);
  {
    // The link alone, never what it leads to: a junction is an empty directory to remove().
    std::error_code ec;
    REQUIRE(std::filesystem::remove(fs_path(doc + "/out"), ec));
  }
  REQUIRE(make_dir_link(target, doc + "/out"));
  REQUIRE(remove_directory_recursive(target) == Status::Ok);
  {
    Roots r;
    REQUIRE(r.add("doc", doc, false) == Status::Ok);
    CHECK(r.add_inside("out", "doc", "out") == Status::NotDirectory);
  }
  // Not one plain name, or no such parent.
  Roots r;
  REQUIRE(r.add("doc", doc, false) == Status::Ok);
  CHECK(r.add_inside("out", "doc", "a/b") == Status::InvalidArgument);
  CHECK(r.add_inside("out", "doc", "..") == Status::InvalidArgument);
  CHECK(r.add_inside("out", "nope", "out") == Status::InvalidArgument);
}

TEST_CASE("roots: device names are refused in any component, case and suffix") {
  Fixture f;
  for (const char* p :
       {"NUL",    "nul",     "con",      "CON.txt",       "con .txt",    "Aux.tar.gz",
        "prn",    "com1",    "COM1.txt", "com0",          "lpt9",        "LPT1.log",
        "CONIN$", "conout$", "CLOCK$",   "sub/NUL/x.txt", "COM\xC2\xB9", "lpt\xC2\xB3.txt",
        "NUL.",   "con  "}) {
    INFO(p);
    const Refusal why = f.refuse(p, Access::write);
    CHECK((why == Refusal::device || why == Refusal::trailing_dot_or_space));
  }
  CHECK(f.refuse("com1.txt", Access::write) == Refusal::device);
  // Names that only look like one are fine.
  f.allow("console.txt", Access::write);
  f.allow("com10", Access::write);
  f.allow("nul_file", Access::write);
}

TEST_CASE("roots: components ending in a dot or a space are refused") {
  Fixture f;
  CHECK(f.refuse("inside.txt.") == Refusal::trailing_dot_or_space);
  CHECK(f.refuse("inside.txt ") == Refusal::trailing_dot_or_space);
  CHECK(f.refuse(".. /outside/secret.txt") == Refusal::trailing_dot_or_space);
  CHECK(f.refuse("../outside/secret.txt.") == Refusal::trailing_dot_or_space);
  CHECK(f.refuse("...") == Refusal::trailing_dot_or_space);
  CHECK(f.refuse("sub./x") == Refusal::trailing_dot_or_space);
  f.allow(".", Access::read);
  f.allow("sub/./x.txt", Access::write);
}

TEST_CASE("roots: a read-only root refuses writes and directories") {
  Fixture f;
  const std::string ro = f.tmp.path() + "/ro";
  make_dir(ro);
  REQUIRE(f.roots.add("ro", ro, false) == Status::Ok);
  Admitted a;
  Refusal why = Refusal::none;
  CHECK(f.roots.admit("ro://x.txt", {}, Access::read, a, why));
  CHECK_FALSE(f.roots.admit("ro://x.txt", {}, Access::write, a, why));
  CHECK(why == Refusal::read_only_root);
  CHECK_FALSE(f.roots.admit("ro://d", {}, Access::make_directory, a, why));
  CHECK(why == Refusal::read_only_root);
  // A read root must exist; a writable one is made.
  CHECK(f.roots.add("missing", f.tmp.path() + "/missing", false) == Status::NotFound);
  CHECK(f.roots.add("made", f.tmp.path() + "/made", true) == Status::Ok);
  CHECK(exists(f.tmp.path() + "/made"));
  // Names are checked and unique.
  CHECK(f.roots.add("Bad-Name", ro, false) == Status::InvalidArgument);
  CHECK(f.roots.add("ro", ro, false) == Status::InvalidArgument);
}

TEST_CASE("roots: the most specific root answers, so out/ under a read document is writable") {
  TempDir tmp;
  const std::string doc = tmp.path() + "/doc";
  make_dir(doc);
  put(doc + "/manifest.json", "{}");
  Roots roots;
  REQUIRE(roots.add("doc", doc, false) == Status::Ok);
  REQUIRE(roots.add("out", doc + "/out", true, /*make=*/false) == Status::Ok);
  CHECK_FALSE(exists(doc + "/out"));  // made by the first write, not at start
  REQUIRE(roots.protect(doc) == Status::Ok);
  Admitted a;
  Refusal why = Refusal::none;
  // Named before its directory exists: a root made by its first write is present all the same.
  CHECK(roots.admit("out://captures/b.png", {}, Access::write, a, why));
  CHECK(a.portable() == "out://captures/b.png");
  CHECK(roots.admit("out", doc, Access::make_directory, a, why));
  CHECK(roots.admit("out/captures/a.png", doc, Access::write, a, why));
  CHECK(a.portable() == "out://captures/a.png");
  CHECK(roots.admit("manifest.json", doc, Access::read, a, why));
  CHECK(a.portable() == "doc://manifest.json");
}

// The most specific root that *allowed* the access answered, so a read-only root inside a
// writable one shielded nothing: a write to refs://x was admitted under work.
TEST_CASE("roots: a read-only root inside a writable one stays read-only") {
  Fixture f;
  make_dir(f.root + "/refs");
  put(f.root + "/refs/a.txt", "a");
  REQUIRE(f.roots.add("refs", f.root + "/refs", false) == Status::Ok);
  CHECK(f.refuse("refs://a.txt", Access::write) == Refusal::read_only_root);
  CHECK(f.refuse("refs://new.txt", Access::write) == Refusal::read_only_root);
  CHECK(f.refuse(f.root + "/refs/a.txt", Access::write) == Refusal::read_only_root);
  CHECK(f.refuse("work://refs/sub", Access::make_directory) == Refusal::read_only_root);
  // Read as before, answered by the more specific root; and the rest of work writes.
  CHECK(f.allow("work://refs/a.txt").portable() == "refs://a.txt");
  CHECK(f.allow("work://other.txt", Access::write).root == "work");
}

TEST_CASE("roots: an open document's own files are never written, out/ apart") {
  Fixture f;
  const std::string doc = f.root + "/docs/world";
  make_dir(doc + "/layers");
  put(doc + "/manifest.json", "{}");
  put(doc + "/layers/base.json", "{}");
  // Before it is protected, a writable root reaches it: that is what protect() closes.
  f.allow(doc + "/layers/base.json", Access::write);
  REQUIRE(f.roots.protect(doc) == Status::Ok);
  CHECK(f.refuse(doc + "/layers/base.json", Access::write) == Refusal::document);
  CHECK(f.refuse(doc + "/manifest.json", Access::write) == Refusal::document);
  CHECK(f.refuse(doc + "/journal.jsonl", Access::write) == Refusal::document);
  CHECK(f.refuse(doc + "/layers/x", Access::make_directory) == Refusal::document);
  CHECK(f.refuse(doc + "/out", Access::write) == Refusal::document);  // a file named out
#if ENGINE_PLATFORM_WINDOWS
  // Another case is the same directory on Windows (on Linux it is another, missing, one).
  CHECK(f.refuse("work://docs/WORLD/layers/base.json", Access::write) == Refusal::document);
#endif
  f.allow(doc + "/out", Access::make_directory);
  f.allow(doc + "/out/a.png", Access::write);
  f.allow(doc + "/layers/base.json", Access::read);
  f.roots.release(doc);
  f.allow(doc + "/layers/base.json", Access::write);
}

TEST_CASE("roots: a served directory is never written, and no writable root may hold it") {
  TempDir tmp;
  const std::string shell = tmp.path() + "/bin/shell";
  make_dir(shell);
  put(shell + "/index.html", "<html>");
  Roots roots;
  REQUIRE(roots.serve(shell) == Status::Ok);
  CHECK(roots.add("bin", tmp.path() + "/bin", true) == Status::InvalidArgument);
  CHECK(roots.add("shell", shell, true) == Status::InvalidArgument);
  CHECK(roots.add("deep", shell + "/sub", true) == Status::InvalidArgument);
  CHECK(roots.add("bin_ro", tmp.path() + "/bin", false) == Status::Ok);  // reading is fine
  Roots other;
  REQUIRE(other.add("bin", tmp.path() + "/bin", true) == Status::Ok);
  CHECK(other.serve(shell) == Status::InvalidArgument);
  // Through a link inside a writable root that is allowed: the write lands in the served
  // directory and is refused there.
  Roots linked;
  const std::string work = tmp.path() + "/work";
  make_dir(work);
  REQUIRE(linked.add("work", work, true) == Status::Ok);
  REQUIRE(linked.serve(shell) == Status::Ok);
  if (!make_dir_link(shell, work + "/to_shell")) {
    MESSAGE("no directory link could be made here; the link half is skipped");
    return;
  }
  Admitted a;
  Refusal why = Refusal::none;
  // The link leads out of the only writable root, so the shell's index is not a place to write.
  CHECK_FALSE(linked.admit("work://to_shell/index.html", {}, Access::write, a, why));
  CHECK(why == Refusal::outside);
  CHECK_FALSE(linked.admit("work://to_shell/new.html", {}, Access::write, a, why));
  CHECK(why == Refusal::outside);
}

TEST_CASE("roots: a link inside a root that leads out is followed and refused") {
  Fixture f;
  if (!make_dir_link(f.outside, f.root + "/escape")) {
    MESSAGE("no directory link could be made here; skipped");
    return;
  }
  CHECK(f.refuse("escape/secret.txt") == Refusal::outside);
  CHECK(f.refuse("work://escape/secret.txt") == Refusal::outside);
  CHECK(f.refuse("escape/new.txt", Access::write) == Refusal::outside);
  CHECK(f.refuse("escape/d", Access::make_directory) == Refusal::outside);
  // A link that stays inside is followed and admitted, answered by where it leads.
  make_dir(f.root + "/real");
  REQUIRE(make_dir_link(f.root + "/real", f.root + "/alias"));
  const Admitted a = f.allow("alias/x.txt", Access::read);
  CHECK(a.relative == "real/x.txt");
  // `root/link/..` is collapsed lexically and then opened as checked: it is the root.
  const Admitted b = f.allow("escape/../inside.txt");
  CHECK(b.relative == "inside.txt");
}

TEST_CASE("roots: a file symbolic link inside a root that leads out") {
  Fixture f;
  if (!make_file_symlink(f.outside + "/secret.txt", f.root + "/secret_link.txt")) {
    MESSAGE("no symbolic link could be made here (Windows without developer mode); skipped");
    return;
  }
  CHECK(f.refuse("secret_link.txt") == Refusal::outside);
  CHECK(f.refuse("secret_link.txt", Access::write) == Refusal::outside);
}

TEST_CASE("roots: a dangling link is not a missing name") {
  Fixture f;
  const std::string target = f.outside + "/later";
  make_dir(target);
  if (!make_dir_link(target, f.root + "/dangling")) {
    MESSAGE("no directory link could be made here; skipped");
    return;
  }
  REQUIRE(remove_directory_recursive(target) == Status::Ok);
  // Opened for a write, the link would make its target outside the root.
  CHECK(f.refuse("dangling", Access::make_directory) == Refusal::dangling_link);
  CHECK(f.refuse("dangling/x.txt", Access::write) == Refusal::dangling_link);
  CHECK_FALSE(exists(target));
}

TEST_CASE("roots: a write onto an existing link inside the root is refused") {
  Fixture f;
  make_dir(f.root + "/real");
  put(f.root + "/real/a.txt", "a");
  if (!make_file_symlink(f.root + "/real/a.txt", f.root + "/a_link.txt")) {
    MESSAGE("no symbolic link could be made here (Windows without developer mode); skipped");
  } else {
    CHECK(f.refuse("a_link.txt", Access::write) == Refusal::final_link);
    f.allow("a_link.txt", Access::read);
  }
  if (make_dir_link(f.root + "/real", f.root + "/dir_link")) {
    CHECK(f.refuse("dir_link", Access::write) == Refusal::final_link);
  }
}

TEST_CASE("roots: a write onto a hard link replaces it and leaves the file outside unchanged") {
  Fixture f;
  std::error_code ec;
  std::filesystem::create_hard_link(fs_path(f.outside + "/secret.txt"),
                                    fs_path(f.root + "/hard.txt"), ec);
  if (ec) {
    MESSAGE("no hard link could be made here; skipped");
    return;
  }
  // A hard link is the file: it is under the root, and admitted.
  const Admitted a = f.allow("hard.txt", Access::write);
  // The write the host makes is by rename, so it replaces the link rather than writing through it.
  REQUIRE(write_file_atomic(a.native, "replaced") == Status::Ok);
  std::string text;
  REQUIRE(read_file(f.outside + "/secret.txt", text) == Status::Ok);
  CHECK(text == "secret");
  REQUIRE(read_file(f.root + "/hard.txt", text) == Status::Ok);
  CHECK(text == "replaced");
}

TEST_CASE("roots: a write onto a directory, and a directory onto a file") {
  Fixture f;
  make_dir(f.root + "/d");
  CHECK(f.refuse("d", Access::write) == Refusal::not_regular);
  CHECK(f.refuse("inside.txt", Access::make_directory) == Refusal::not_regular);
  f.allow("d", Access::make_directory);
}

TEST_CASE("roots: a root that is itself a link is its target, fixed at start") {
  TempDir tmp;
  const std::string a = tmp.path() + "/a";
  const std::string b = tmp.path() + "/b";
  make_dir(a);
  make_dir(b);
  put(a + "/x.txt", "a");
  put(b + "/x.txt", "b");
  const std::string link = tmp.path() + "/link";
  if (!make_dir_link(a, link)) {
    MESSAGE("no directory link could be made here; skipped");
    return;
  }
  Roots roots;
  REQUIRE(roots.add("r", link, false) == Status::Ok);
  REQUIRE(roots.roots().size() == 1);
  CHECK(roots.roots()[0].linked);
  CHECK(roots.roots()[0].canonical == canon(a));
  Admitted out;
  Refusal why = Refusal::none;
  CHECK(roots.admit(link + "/x.txt", {}, Access::read, out, why));
  CHECK(out.native == canon(a) + "/x.txt");
  // The link retargeted later is not followed: the root is still a, and b is outside.
#if ENGINE_PLATFORM_WINDOWS
  REQUIRE(RemoveDirectoryW(wide(link).c_str()));
#else
  // The link itself: remove_file follows it to the directory it names and refuses that.
  std::error_code removed;
  REQUIRE(std::filesystem::remove(std::filesystem::path(link), removed));
#endif
  REQUIRE(make_dir_link(b, link));
  CHECK_FALSE(roots.admit(link + "/x.txt", {}, Access::read, out, why));
  CHECK(why == Refusal::outside);
  CHECK(roots.admit("r://x.txt", {}, Access::read, out, why));
  CHECK(out.native == canon(a) + "/x.txt");
}

#if ENGINE_PLATFORM_WINDOWS
TEST_CASE("roots: Windows spellings of one directory are one root") {
  Fixture f;
  // Another case of the same directory.
  std::string upper = f.root;
  for (char& c : upper)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
  const Admitted a = f.allow(upper + "/INSIDE.TXT");
  CHECK(a.relative == "inside.txt");  // the case on disk, not the client's
  std::string lower = f.root;
  for (char& c : lower)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  f.allow(lower + "/inside.txt");
  // Backslashes are separators.
  std::string back = f.root + "\\inside.txt";
  f.allow(back);
  // An 8.3 short name for a long one, where the volume makes them.
  const std::string long_dir = f.root + "/a_rather_long_directory_name";
  make_dir(long_dir);
  wchar_t buf[1024];
  const DWORD n = GetShortPathNameW(wide(long_dir).c_str(), buf, 1024);
  const std::wstring short_w(buf, n);
  if (n == 0 || short_w.find(L"~") == std::wstring::npos) {
    MESSAGE("this volume makes no 8.3 names; the short-name half is skipped");
  } else {
    std::string short_path(
        static_cast<usize>(WideCharToMultiByte(CP_UTF8, 0, short_w.c_str(), static_cast<int>(n),
                                               nullptr, 0, nullptr, nullptr)),
        '\0');
    WideCharToMultiByte(CP_UTF8, 0, short_w.c_str(), static_cast<int>(n), short_path.data(),
                        static_cast<int>(short_path.size()), nullptr, nullptr);
    const Admitted s = f.allow(short_path + "/x.png", Access::write);
    CHECK(s.relative == "a_rather_long_directory_name/x.png");
    // And a short name for the outside directory is still outside.
    const DWORD m = GetShortPathNameW(wide(f.outside).c_str(), buf, 1024);
    if (m != 0) {
      std::string o(static_cast<usize>(WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(m),
                                                           nullptr, 0, nullptr, nullptr)),
                    '\0');
      WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(m), o.data(),
                          static_cast<int>(o.size()), nullptr, nullptr);
      CHECK(f.refuse(o + "/secret.txt") == Refusal::outside);
    }
  }
}
#endif

TEST_CASE("roots: check_path_text is the lexical half alone") {
  CHECK(check_path_text("a/b.png") == Refusal::none);
  CHECK(check_path_text("C:/a/b.png") == Refusal::none);
  CHECK(check_path_text("..") == Refusal::none);
  CHECK(check_path_text(".. ") == Refusal::trailing_dot_or_space);
  CHECK(check_path_text("CON") == Refusal::device);
  CHECK(check_path_text("a:b") == Refusal::stream);
}
