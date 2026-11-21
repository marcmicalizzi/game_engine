// The host's file roots (roots.h; docs/subsystems/io.md, "Roots"; ADR-0071).
#include <foundation/io/roots.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#if ENGINE_PLATFORM_WINDOWS
// The final-path calls: `std::filesystem::canonical` neither refuses a prefix with no DOS name nor
// says which error it met, and the rules need both.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <sys/stat.h>
#endif

namespace engine::io {

namespace {

#if ENGINE_PLATFORM_WINDOWS
constexpr usize k_max_path_bytes = 32767;
#else
constexpr usize k_max_path_bytes = PATH_MAX;
#endif

bool valid_utf8(std::string_view s) noexcept {
  usize i = 0;
  while (i < s.size()) {
    const u8 c = static_cast<u8>(s[i]);
    if (c < 0x80) {
      ++i;
      continue;
    }
    usize n = 0;
    u32 cp = 0;
    if ((c & 0xE0) == 0xC0) {
      n = 1;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      n = 2;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      n = 3;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + n >= s.size()) return false;  // the sequence runs past the end
    for (usize k = 1; k <= n; ++k) {
      const u8 cc = static_cast<u8>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    // Overlong forms, surrogates and code points past U+10FFFF are not UTF-8.
    if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000)) return false;
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;
    if (cp > 0x10FFFF) return false;
    i += n + 1;
  }
  return true;
}

char ascii_upper(char c) noexcept { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; }

bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (usize i = 0; i < a.size(); ++i) {
    if (ascii_upper(a[i]) != ascii_upper(b[i])) return false;
  }
  return true;
}

// A component Windows opens as a device, wherever it stands in a path: the name with its trailing
// dots and spaces and everything from its first dot removed, compared case-insensitively.
bool is_device_name(std::string_view component) noexcept {
  while (!component.empty() && (component.back() == '.' || component.back() == ' '))
    component.remove_suffix(1);
  const usize dot = component.find('.');
  if (dot != std::string_view::npos) component = component.substr(0, dot);
  // Spaces before the dot are stripped too ("CON .txt" is CON).
  while (!component.empty() && component.back() == ' ')
    component.remove_suffix(1);
  static constexpr std::string_view k_plain[] = {"CON",    "PRN",     "AUX",   "NUL",
                                                 "CONIN$", "CONOUT$", "CLOCK$"};
  for (const std::string_view name : k_plain) {
    if (iequals(component, name)) return true;
  }
  if (component.size() < 4) return false;
  const std::string_view head = component.substr(0, 3);
  if (!iequals(head, "COM") && !iequals(head, "LPT")) return false;
  const std::string_view tail = component.substr(3);
  if (tail.size() == 1 && tail[0] >= '0' && tail[0] <= '9') return true;
  // The superscript digits Windows also treats as ports: U+00B9, U+00B2, U+00B3.
  return tail == "\xC2\xB9" || tail == "\xC2\xB2" || tail == "\xC2\xB3";
}

std::string slashes(std::string_view path) {
  std::string s(path);
  for (char& c : s) {
    if (c == '\\') c = '/';
  }
  return s;
}

bool is_drive_prefix(std::string_view s) noexcept {
  return s.size() >= 3 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) &&
         s[1] == ':' && s[2] == '/';
}

bool is_unc(std::string_view s) noexcept { return s.size() >= 2 && s[0] == '/' && s[1] == '/'; }

// The root of an absolute path ("C:/", "/", "//server/share/") and the components after it, with
// "." dropped and ".." collapsed. False when a ".." would climb above the root.
bool split_absolute(std::string_view s, std::string& root, Vector<std::string>& components) {
  usize pos = 0;
  if (is_drive_prefix(s)) {
    root = std::string(1, ascii_upper(s[0])) + ":/";
    pos = 3;
  } else if (is_unc(s)) {
    // //server/share is the root; both must be present.
    const usize server_end = s.find('/', 2);
    if (server_end == std::string_view::npos || server_end == 2) return false;
    usize share_end = s.find('/', server_end + 1);
    if (share_end == std::string_view::npos) share_end = s.size();
    if (share_end == server_end + 1) return false;
    root = std::string(s.substr(0, share_end)) + "/";
    pos = share_end;
  } else if (!s.empty() && s[0] == '/') {
    root = "/";
    pos = 1;
  } else {
    return false;
  }
  components.clear();
  while (pos <= s.size()) {
    usize end = s.find('/', pos);
    if (end == std::string_view::npos) end = s.size();
    const std::string_view c = s.substr(pos, end - pos);
    if (c.empty() || c == ".") {
      // nothing
    } else if (c == "..") {
      if (components.empty()) return false;
      components.pop_back();
    } else {
      components.push_back(std::string(c));
    }
    pos = end + 1;
  }
  return true;
}

std::string join_components(const std::string& root, const Vector<std::string>& components,
                            u32 count) {
  std::string out = root;
  for (u32 i = 0; i < count; ++i) {
    if (!out.empty() && out.back() != '/') out += '/';
    out += components[i];
  }
  return out;
}

std::string current_directory() {
  std::error_code ec;
  const std::filesystem::path p = std::filesystem::current_path(ec);
  if (ec) return {};
  const std::u8string s = p.u8string();
  return slashes(std::string_view(reinterpret_cast<const char*>(s.data()), s.size()));
}

bool is_absolute_spelling(std::string_view s) noexcept {
  return is_drive_prefix(s) || (!s.empty() && s[0] == '/');
}

enum class Probe : u8 { ok, missing, refused, no_dos_name };

#if ENGINE_PLATFORM_WINDOWS
bool to_wide(std::string_view s, std::wstring& out) {
  out.clear();
  if (s.empty()) return true;
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return false;
  out.resize(static_cast<usize>(n));
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                      out.data(), n);
  for (wchar_t& c : out) {
    if (c == L'/') c = L'\\';
  }
  return true;
}

std::string from_wide(const wchar_t* s, usize n) {
  if (n == 0) return {};
  const int bytes =
      WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<usize>(bytes > 0 ? bytes : 0), '\0');
  if (bytes > 0)
    WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(n), out.data(), bytes, nullptr, nullptr);
  return out;
}

// What the system says an existing path is: opened (links, junctions and mount points followed)
// and named by its handle.
Probe final_path(const std::string& absolute, std::string& out) {
  std::wstring w;
  if (!to_wide(absolute, w)) return Probe::refused;
  const HANDLE h = CreateFileW(w.c_str(), FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_DIRECTORY ||
        e == ERROR_BAD_NETPATH || e == ERROR_BAD_NET_NAME) {
      return Probe::missing;
    }
    return Probe::refused;
  }
  std::wstring buf(512, L'\0');
  DWORD n = GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()),
                                      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (n >= buf.size()) {
    buf.assign(static_cast<usize>(n) + 1, L'\0');
    n = GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()),
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  }
  const DWORD e = n == 0 ? GetLastError() : ERROR_SUCCESS;
  CloseHandle(h);
  if (n == 0 || n >= buf.size())
    return e == ERROR_PATH_NOT_FOUND ? Probe::no_dos_name : Probe::refused;
  std::string s = slashes(from_wide(buf.data(), n));
  if (s.starts_with("//?/UNC/")) {
    s = "//" + s.substr(8);
  } else if (s.starts_with("//?/")) {
    s = s.substr(4);
  } else {
    return Probe::refused;
  }
  if (s.size() == 2 && s[1] == ':') s += '/';
  out = std::move(s);
  return Probe::ok;
}

// Whether an absolute path names a drive the system calls remote (a mapped network drive). Asked
// of the drive's letter alone, which the system answers from its own table of drives without
// opening anything on the share.
bool remote_drive(std::string_view s) noexcept {
  if (!is_drive_prefix(s)) return false;
  const wchar_t drive[4] = {static_cast<wchar_t>(s[0]), L':', L'\\', L'\0'};
  return GetDriveTypeW(drive) == DRIVE_REMOTE;
}

// Whether something is at this path without following its last component: a dangling link.
// `reparse` says whether that something is a link, a junction or another reparse point.
bool exists_unfollowed(const std::string& absolute, bool& reparse, bool& directory) {
  std::wstring w;
  if (!to_wide(absolute, w)) return false;
  const DWORD a = GetFileAttributesW(w.c_str());
  if (a == INVALID_FILE_ATTRIBUTES) return false;
  reparse = (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  directory = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
  return true;
}
#else
// A network share is reached on Linux through a mount the operator made, never through a name in
// the path, so no spelling of a client's makes the host connect anywhere.
bool remote_drive(std::string_view) noexcept { return false; }

Probe final_path(const std::string& absolute, std::string& out) {
  char buf[PATH_MAX];
  if (::realpath(absolute.c_str(), buf) == nullptr) {
    return errno == ENOENT || errno == ENOTDIR ? Probe::missing : Probe::refused;
  }
  out = buf;
  return Probe::ok;
}

bool exists_unfollowed(const std::string& absolute, bool& reparse, bool& directory) {
  struct stat st{};
  if (::lstat(absolute.c_str(), &st) != 0) return false;
  reparse = S_ISLNK(st.st_mode);
  directory = S_ISDIR(st.st_mode);
  return true;
}
#endif

// Steps 7 and 8 over a path already joined and made absolute. `lexical` is the collapsed absolute
// spelling (what a write's last component is checked on); `missing` how many trailing components
// do not exist.
Refusal resolve(std::string_view absolute, std::string& lexical, std::string& out, u32& missing) {
  std::string root;
  Vector<std::string> components;
  if (!split_absolute(absolute, root, components)) return Refusal::above_root;
  lexical = join_components(root, components, components.size());
  u32 have = components.size();
  for (;;) {
    const std::string prefix = join_components(root, components, have);
    std::string canonical;
    const Probe p = final_path(prefix, canonical);
    if (p == Probe::ok) {
      if (have < components.size()) {
        // The first missing component must not exist unfollowed: a link to nowhere, opened for a
        // write, would make its target wherever it points.
        bool reparse = false;
        bool directory = false;
        std::string first = canonical;
        if (first.back() != '/') first += '/';
        first += components[have];
        if (exists_unfollowed(first, reparse, directory)) return Refusal::dangling_link;
      }
      out = std::move(canonical);
      for (u32 i = have; i < components.size(); ++i) {
        if (out.back() != '/') out += '/';
        out += components[i];
      }
      missing = components.size() - have;
      return Refusal::none;
    }
    if (p == Probe::no_dos_name) return Refusal::no_dos_name;
    if (p == Probe::refused || have == 0) return Refusal::cannot_resolve;
    --have;
  }
}

std::string make_absolute(std::string_view path, std::string_view base, Refusal& why) {
  std::string p = slashes(path);
  if (is_absolute_spelling(p)) return p;
  std::string b = slashes(base);
  if (b.empty()) {
    why = Refusal::no_base;
    return {};
  }
  if (!is_absolute_spelling(b)) {
    const std::string cwd = current_directory();
    if (cwd.empty()) {
      why = Refusal::cannot_resolve;
      return {};
    }
    b = cwd + "/" + b;
  }
  if (b.back() != '/') b += '/';
  return b + p;
}

#if ENGINE_PLATFORM_WINDOWS
bool same_prefix_ignore_case(std::string_view a, std::string_view b) noexcept {
  std::wstring wa;
  std::wstring wb;
  if (!to_wide(a, wa) || !to_wide(b, wb) || wa.size() != wb.size()) return false;
  return CompareStringOrdinal(wa.c_str(), static_cast<int>(wa.size()), wb.c_str(),
                              static_cast<int>(wb.size()), TRUE) == CSTR_EQUAL;
}
#endif

}  // namespace

const char* access_name(Access access) noexcept {
  switch (access) {
    case Access::read: return "read";
    case Access::write: return "write";
    case Access::make_directory: return "make_directory";
  }
  return "?";
}

const char* refusal_name(Refusal refusal) noexcept {
  switch (refusal) {
    case Refusal::none: return "none";
    case Refusal::empty: return "empty";
    case Refusal::too_long: return "too_long";
    case Refusal::not_utf8: return "not_utf8";
    case Refusal::control: return "control";
    case Refusal::reserved_character: return "reserved_character";
    case Refusal::stream: return "stream";
    case Refusal::drive_relative: return "drive_relative";
    case Refusal::namespace_prefix: return "namespace_prefix";
    case Refusal::unc: return "unc";
    case Refusal::device: return "device";
    case Refusal::trailing_dot_or_space: return "trailing_dot_or_space";
    case Refusal::no_base: return "no_base";
    case Refusal::above_root: return "above_root";
    case Refusal::cannot_resolve: return "cannot_resolve";
    case Refusal::no_dos_name: return "no_dos_name";
    case Refusal::dangling_link: return "dangling_link";
    case Refusal::outside: return "outside";
    case Refusal::read_only_root: return "read_only_root";
    case Refusal::document: return "document";
    case Refusal::served: return "served";
    case Refusal::final_link: return "final_link";
    case Refusal::not_regular: return "not_regular";
    case Refusal::unknown_root: return "unknown_root";
    case Refusal::absolute_relative: return "absolute_relative";
    case Refusal::absolute_stored: return "absolute_stored";
  }
  return "?";
}

bool valid_root_name(std::string_view name) noexcept {
  if (name.empty() || name.size() > 32) return false;
  for (const char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

bool path_is_under(std::string_view path, std::string_view dir) noexcept {
  if (dir.empty() || path.size() < dir.size()) return false;
  const std::string_view head = path.substr(0, dir.size());
#if ENGINE_PLATFORM_WINDOWS
  if (!same_prefix_ignore_case(head, dir)) return false;
#else
  if (head != dir) return false;
#endif
  if (path.size() == dir.size()) return true;
  return dir.back() == '/' || path[dir.size()] == '/';
}

Refusal check_path_text(std::string_view path) noexcept {
  // 1. Text.
  if (path.empty()) return Refusal::empty;
  if (path.size() > k_max_path_bytes) return Refusal::too_long;
  if (!valid_utf8(path)) return Refusal::not_utf8;
  for (const char ch : path) {
    const u8 c = static_cast<u8>(ch);
    if (c < 0x20 || c == 0x7F) return Refusal::control;
  }
  // 2. Characters Windows reserves.
  for (const char c : path) {
    if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*')
      return Refusal::reserved_character;
  }
  const std::string s = slashes(path);
  // 4. Namespaces, before ':' (\\?\C:\ would pass as a drive otherwise). '?' is already refused,
  // so \\?\ and \??\ never reach here; \\.\ is what is left.
  if (s.starts_with("//./") || s == "//.") return Refusal::namespace_prefix;
  // 3. ':' only as a drive's.
  for (usize i = 0; i < s.size(); ++i) {
    if (s[i] != ':') continue;
    if (i == 1 && is_drive_prefix(s)) continue;
    return Refusal::stream;
  }
#if ENGINE_PLATFORM_WINDOWS
  // "/x" is the root of whichever drive is current: not a place a client can name.
  if (s[0] == '/' && !is_unc(s)) return Refusal::drive_relative;
#endif
  // 6. Components.
  usize pos = 0;
  while (pos <= s.size()) {
    usize end = s.find('/', pos);
    if (end == std::string::npos) end = s.size();
    const std::string_view c = std::string_view(s).substr(pos, end - pos);
    pos = end + 1;
    if (c.empty() || c == "." || c == "..") continue;
    if (c.size() == 2 && c[1] == ':') continue;  // the drive
    if (c.back() == '.' || c.back() == ' ') return Refusal::trailing_dot_or_space;
    if (is_device_name(c)) return Refusal::device;
  }
  return Refusal::none;
}

Refusal canonical_path(std::string_view path, std::string_view base, std::string& out) {
  Refusal why = Refusal::none;
  const std::string absolute = make_absolute(path, base, why);
  if (why != Refusal::none) return why;
  std::string lexical;
  u32 missing = 0;
  return resolve(absolute, lexical, out, missing);
}

const Root* Roots::find(std::string_view name) const noexcept {
  for (const Root& r : roots_) {
    if (r.name == name) return &r;
  }
  return nullptr;
}

Status Roots::add(std::string_view name, std::string_view dir, bool writable, bool make,
                  Refusal* refused) {
  Refusal ignored = Refusal::none;
  Refusal& step = refused != nullptr ? *refused : ignored;
  step = Refusal::none;
  if (!valid_root_name(name) || find(name) != nullptr) return Status::InvalidArgument;
  if (dir.empty()) return Status::InvalidPath;
  Refusal why = Refusal::none;
  const std::string absolute = make_absolute(dir, {}, why);
  std::string abs = absolute;
  if (why == Refusal::no_base) {
    const std::string cwd = current_directory();
    if (cwd.empty()) return Status::InvalidPath;
    abs = cwd + "/" + slashes(dir);
  }
  // A network share, by its spelling, before anything is made or opened on it.
  if (is_unc(abs) || remote_drive(abs)) {
    step = Refusal::unc;
    return Status::InvalidPath;
  }
  if (writable && make) {
    if (const Status s = make_directories(abs); s != Status::Ok) return s;
  }
  std::string lexical;
  std::string canonical;
  u32 missing = 0;
  if (const Refusal r = resolve(abs, lexical, canonical, missing); r != Refusal::none) {
    step = r;
    return Status::InvalidPath;
  }
  // And by what the system says it is: a link to a share, or a drive mapped to one.
  if (is_unc(canonical) || remote_drive(canonical)) {
    step = Refusal::unc;
    return Status::InvalidPath;
  }
  if (missing > 0 && (!writable || make)) return Status::NotFound;
  for (const std::string& s : served_) {
    if (writable && (path_is_under(s, canonical) || path_is_under(canonical, s)))
      return Status::InvalidArgument;
  }
  Root root;
  root.name = std::string(name);
  root.spelled = std::string(dir);
  root.canonical = canonical;
  root.writable = writable;
  root.made_on_write = writable && !make;
#if ENGINE_PLATFORM_WINDOWS
  root.linked =
      !(lexical.size() == canonical.size() && same_prefix_ignore_case(lexical, canonical));
#else
  root.linked = lexical != canonical;
#endif
  roots_.push_back(std::move(root));
  return Status::Ok;
}

Status Roots::add_inside(std::string_view name, std::string_view parent, std::string_view child) {
  const Root* p = find(parent);
  if (p == nullptr || child.empty() || child == "." || child == ".." ||
      child.find_first_of("/\\") != std::string_view::npos ||
      check_path_text(child) != Refusal::none)
    return Status::InvalidArgument;
  // A copy: `add` grows the set, which moves the root `p` points at.
  const std::string parent_dir = p->canonical;
  std::string at = parent_dir;
  if (at.back() != '/') at += '/';
  at += child;
  bool reparse = false;
  bool directory = false;
  // A link (live or dangling), a junction, any reparse point, or a file: not the document's own.
  if (exists_unfollowed(at, reparse, directory) && (reparse || !directory))
    return Status::NotDirectory;
  if (const Status s = add(name, at, true, false); s != Status::Ok) return s;
  // What the system says it is must be the child itself, under the parent: a link made between
  // the look above and the resolve in `add` lands here.
  const Root& made = roots_.back();
  if (made.linked || !path_is_under(made.canonical, parent_dir) ||
      made.canonical.size() <= parent_dir.size()) {
    roots_.pop_back();
    return Status::NotDirectory;
  }
  return Status::Ok;
}

Status Roots::serve(std::string_view dir) {
  std::string canonical;
  if (canonical_path(dir, current_directory(), canonical) != Refusal::none)
    return Status::InvalidPath;
  for (const Root& r : roots_) {
    if (r.writable &&
        (path_is_under(canonical, r.canonical) || path_is_under(r.canonical, canonical)))
      return Status::InvalidArgument;
  }
  served_.push_back(std::move(canonical));
  return Status::Ok;
}

Status Roots::protect(std::string_view document_dir) {
  std::string canonical;
  if (canonical_path(document_dir, current_directory(), canonical) != Refusal::none)
    return Status::InvalidPath;
  // Held once, however often it is asked and however it is spelled: a second holder counts, so the
  // list every write's admission walks is as long as the documents open, never the opens made.
  for (u32 i = 0; i < documents_.size(); ++i) {
    if (path_is_under(documents_[i], canonical) && path_is_under(canonical, documents_[i])) {
      ++document_holds_[i];
      return Status::Ok;
    }
  }
  documents_.push_back(std::move(canonical));
  document_holds_.push_back(1);
  return Status::Ok;
}

void Roots::release(std::string_view document_dir) {
  std::string canonical;
  if (canonical_path(document_dir, current_directory(), canonical) != Refusal::none) return;
  for (u32 i = 0; i < documents_.size(); ++i) {
    if (path_is_under(documents_[i], canonical) && path_is_under(canonical, documents_[i])) {
      if (--document_holds_[i] == 0) {
        documents_.erase_at(i);
        document_holds_.erase_at(i);
      }
      return;
    }
  }
}

Refusal check_stored_path_form(std::string_view path) noexcept {
  const usize scheme_end = path.find("://");
  if (scheme_end != std::string_view::npos && valid_root_name(path.substr(0, scheme_end))) {
    const std::string_view rest = path.substr(scheme_end + 3);
    // `<root>://` alone is the root's directory, never a file a record names (T174).
    if (rest.empty()) return Refusal::empty;
    const std::string rel = slashes(rest);
    if (is_absolute_spelling(rel) || (rel.size() >= 2 && rel[1] == ':'))
      return Refusal::absolute_relative;
    return check_path_text(rest);
  }
  const Refusal text = check_path_text(path);
  // Windows' text rules call `/x` drive-relative; stored, it is absolute on every system, and a
  // record is refused for the same reason wherever it is written.
  if (text == Refusal::drive_relative) return Refusal::absolute_stored;
  if (text != Refusal::none) return text;
  // After the text rules, so a stream (`C:x`) is a stream; what is left with a colon second is a
  // drive (`C:/x`), and a leading separator is a volume's root or a share (`/x`, `\\server`).
  const std::string spelled = slashes(path);
  if (is_absolute_spelling(spelled) || (spelled.size() >= 2 && spelled[1] == ':'))
    return Refusal::absolute_stored;
  return Refusal::none;
}

bool admit_stored_path(const Roots& roots, std::string_view path, std::string_view document_dir,
                       Admitted& out, Refusal& why) {
  out = Admitted{};
  why = check_stored_path_form(path);
  if (why != Refusal::none) return false;
  return roots.admit(path, document_dir, Access::read, out, why);
}

namespace {
i32 uri_hex_digit(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
}  // namespace

bool uri_relative_path(std::string_view uri, std::string& out) {
  constexpr usize k_uri_path_max = 1024;
  out.clear();
  // Decoded once: `%255C` is a percent sign and `5C` in a name, never a backslash.
  for (usize i = 0; i < uri.size(); ++i) {
    if (uri[i] == '%' && i + 2 < uri.size() && uri_hex_digit(uri[i + 1]) >= 0 &&
        uri_hex_digit(uri[i + 2]) >= 0) {
      out.push_back(static_cast<char>(uri_hex_digit(uri[i + 1]) * 16 + uri_hex_digit(uri[i + 2])));
      i += 2;
    } else {
      out.push_back(uri[i]);
    }
    if (out.size() > k_uri_path_max) return false;
  }
  if (out.empty() || out.front() == '/') return false;
  // Every byte of the decoded text, wherever it came from: a URI has no backslash, so one is never
  // turned into a separator (every path function here takes it for one, and `\\host` for a share).
  for (const char c : out) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f || c == '\\') return false;
  }
  // Segment by segment into `kept`: a `.` is dropped wherever it is; a `..` at the front is no way
  // out by itself (the caller admits the path through the roots from the naming file's directory,
  // and they decide where a climb leads); a `..` after a named segment is refused, because the
  // admission collapses `a/..` by its text while an open resolves it through whatever `a` is (a
  // link, on Linux), so the digest would hash one file and the importer read another.
  std::string kept;
  bool named = false;
  bool first = true;
  usize start = 0;
  for (;;) {
    const usize slash = out.find('/', start);
    const std::string_view part = std::string_view(out).substr(
        start, slash == std::string::npos ? out.size() - start : slash - start);
    // An empty segment anywhere but after a single trailing separator is refused (`a//b`, `.//x`,
    // `x//`): once a `.` before it is dropped it would be the leading separator refused above.
    if (part.empty() && slash != std::string::npos) return false;
    if (part != ".") {
      if (part == "..") {
        if (named) return false;
      } else if (!part.empty()) {
        named = true;
      }
      // A colon in the first segment kept is a scheme or a drive (`http:`, `C:`); a later one the
      // admission's text rules refuse as a stream.
      if (first && part.find(':') != std::string_view::npos) return false;
      if (!first) kept.push_back('/');
      kept.append(part);
      first = false;
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  if (kept.empty()) return false;
  out = std::move(kept);
  return true;
}

Roots Roots::read_copy() const {
  Roots copy;
  for (const Root& r : roots_)
    copy.roots_.push_back(r);
  return copy;
}

bool Roots::portable(std::string_view native, std::string& out) const {
  const Root* best = nullptr;
  for (const Root& r : roots_) {
    if (path_is_under(native, r.canonical) &&
        (best == nullptr || r.canonical.size() > best->canonical.size()))
      best = &r;
  }
  if (best == nullptr) return false;
  std::string_view rest = native.substr(best->canonical.size());
  while (!rest.empty() && rest.front() == '/')
    rest.remove_prefix(1);
  out = best->name + "://" + std::string(rest);
  return true;
}

bool Roots::may_make_document(std::string_view native) const {
  // `path_is_under(x, native)`: `native` is `x` or holds it.
  for (const Root& r : roots_) {
    if (path_is_under(r.canonical, native)) return false;
  }
  for (const std::string& s : served_) {
    if (path_is_under(s, native)) return false;
  }
  for (const std::string& d : documents_) {
    if (path_is_under(d, native)) return false;
    const std::string out_dir = d + (d.back() == '/' ? "out" : "/out");
    if (path_is_under(native, out_dir) && path_is_under(out_dir, native)) return false;
  }
  return true;
}

bool Roots::admit(std::string_view path, std::string_view base, Access access, Admitted& out,
                  Refusal& why, std::string* resolved) const {
  out = Admitted{};
  // `<root>://<relative>`: the relative part is checked as any client path is, against the root.
  std::string_view named_base;
  const usize scheme_end = path.find("://");
  if (scheme_end != std::string_view::npos && valid_root_name(path.substr(0, scheme_end))) {
    const Root* root = find(path.substr(0, scheme_end));
    std::string probe;
    // A root made by its first write (a document's out/) may not exist yet; any other root whose
    // directory is gone is not present here.
    if (root == nullptr ||
        (!root->made_on_write && final_path(root->canonical, probe) != Probe::ok)) {
      why = Refusal::unknown_root;
      return false;
    }
    path = path.substr(scheme_end + 3);
    named_base = root->canonical;
    if (path.empty()) path = ".";
    const std::string rel = slashes(path);
    if (is_absolute_spelling(rel) || (rel.size() >= 2 && rel[1] == ':')) {
      why = Refusal::absolute_relative;
      return false;
    }
    base = named_base;
  }
  why = check_path_text(path);
  if (why != Refusal::none) return false;
  // 7, and 5: no root is on a network share (`add` refuses one), so a UNC path or one on a remote
  // drive is under no root, and is refused before step 8 opens anything: resolving it would make
  // the host connect, with the operator's credentials, to whichever server the client named.
  const std::string absolute = make_absolute(path, base, why);
  if (why != Refusal::none) return false;
  if (is_unc(absolute) || remote_drive(absolute)) {
    why = Refusal::unc;
    return false;
  }
  std::string lexical;
  std::string canonical;
  u32 missing = 0;
  why = resolve(absolute, lexical, canonical, missing);
  if (why != Refusal::none) return false;
  if (resolved != nullptr) *resolved = canonical;
  // A named path stays under the root it names: "out://../layers" is not a way to spell the
  // document.
  if (!named_base.empty() && !path_is_under(lexical, named_base)) {
    why = Refusal::outside;
    return false;
  }
  // 9. Under a root: the most specific one that holds the path answers, and a write needs it to
  // be writable. So `out/` under a read-only document is writable, and a read-only root the
  // operator named inside a writable one stays read-only: "the most specific root that allows the
  // access" let `--root work=<w>:rw --root refs=<w>/refs` write `refs://x` under `work`.
  const Root* best = nullptr;
  for (const Root& r : roots_) {
    if (!path_is_under(canonical, r.canonical)) continue;
    if (best == nullptr || r.canonical.size() > best->canonical.size()) best = &r;
  }
  if (best == nullptr) {
    why = Refusal::outside;
    return false;
  }
  if (access != Access::read && !best->writable) {
    why = Refusal::read_only_root;
    return false;
  }
  if (access != Access::read) {
    for (const std::string& s : served_) {
      if (path_is_under(canonical, s)) {
        why = Refusal::served;
        return false;
      }
    }
    for (const std::string& d : documents_) {
      if (!path_is_under(canonical, d)) continue;
      const std::string out_dir = d + (d.back() == '/' ? "out" : "/out");
      if (!path_is_under(canonical, out_dir) || canonical.size() == out_dir.size()) {
        // `out/` itself may be made; nothing else of the document may be written.
        if (!(access == Access::make_directory && canonical.size() == out_dir.size() &&
              path_is_under(canonical, out_dir))) {
          why = Refusal::document;
          return false;
        }
      }
    }
    // 10. The last component of a write.
    if (missing == 0) {
      bool reparse = false;
      bool directory = false;
      if (exists_unfollowed(lexical, reparse, directory)) {
        if (reparse) {
          why = Refusal::final_link;
          return false;
        }
        if ((access == Access::write) == directory) {
          why = Refusal::not_regular;
          return false;
        }
      }
    }
  }
  // 11.
  std::string_view rest = std::string_view(canonical).substr(best->canonical.size());
  while (!rest.empty() && rest.front() == '/')
    rest.remove_prefix(1);
  out.root = best->name;
  out.relative = std::string(rest);
  out.native = std::move(canonical);
  return true;
}

}  // namespace engine::io
