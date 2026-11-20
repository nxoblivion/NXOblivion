// reOblivion web (2026-11-17): the game's files in the browser, read from the web server in pieces.
//
// The disc's files (5.9 GB, 166 files) stay where the server has them (nx/web/serve.js --mount /game=<assets>); the
// page never downloads them whole. /game is a read-only tree in Emscripten's WASMFS whose files are this backend's:
// a read is an HTTP Range request made synchronously on the thread that reads (every game thread is a Web Worker,
// where synchronous XMLHttpRequest is allowed), through a block cache. The tree comes from the server's manifest
// (<mount>/.manifest: every file's path and size), so stat and directory listings never touch the network.
//
// So the SDK's host path device (POSIX open/pread/stat/readdir) works unchanged on top of it.

#include "web_game_files.h"

#if defined(__EMSCRIPTEN__)

#include <emscripten.h>
#include <emscripten/wasmfs.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>

// Emscripten's own WASMFS classes (system/lib/wasmfs, on the include path of the web build).
#include "backend.h"
#include "file.h"
#include "memory_backend.h"
#include "wasmfs.h"

// ---------------------------------------------------------------------------------------------------------------------
// The browser side: synchronous XMLHttpRequest, which a Web Worker may make (the page's thread may not, and never
// reads game files: main() runs on a worker, PROXY_TO_PTHREAD).

// Reads [offset, offset + length) of url into dst. Returns the bytes read, or -1.
EM_JS(double, oblivion_web_http_read, (const char *url, double offset, double length, void *dst), {
  var xhr = new XMLHttpRequest();
  xhr.open('GET', UTF8ToString(Number(url)), false);
  xhr.responseType = 'arraybuffer';
  xhr.setRequestHeader('Range', 'bytes=' + offset + '-' + (offset + length - 1));
  try {
    xhr.send(null);
  } catch (e) {
    err('[web files] ' + UTF8ToString(Number(url)) + ': ' + e);
    return -1;
  }
  if (xhr.status !== 206 && !(xhr.status === 200 && offset === 0)) {
    err('[web files] ' + UTF8ToString(Number(url)) + ' at ' + offset + ': HTTP ' + xhr.status);
    return -1;
  }
  var data = new Uint8Array(xhr.response);
  var n = Math.min(data.length, length);
  HEAPU8.set(n === data.length ? data : data.subarray(0, n), Number(dst));
  return n;
});

// GETs url as text (the manifest). Returns a malloc'd string, or null.
EM_JS(double, oblivion_web_http_text_js, (const char *url), {
  var xhr = new XMLHttpRequest();
  xhr.open('GET', UTF8ToString(Number(url)), false);
  try {
    xhr.send(null);
  } catch (e) {
    err('[web files] ' + UTF8ToString(Number(url)) + ': ' + e);
    return 0;
  }
  if (xhr.status !== 200) {
    err('[web files] ' + UTF8ToString(Number(url)) + ': HTTP ' + xhr.status);
    return 0;
  }
  return Number(stringToNewUTF8(xhr.responseText));
});

// The directory the page was loaded from (the worker's own URL is the script's, beside it).
EM_JS(double, oblivion_web_base_url_js, (), {
  var href = (typeof location !== 'undefined') ? location.href : '';
  return Number(stringToNewUTF8(href.substring(0, href.lastIndexOf('/') + 1)));
});

// Pointers come back from JavaScript as numbers (a wasm64 pointer return would have to be a BigInt).
static char *oblivion_web_http_text(const char *url) { return (char *)(uintptr_t)oblivion_web_http_text_js(url); }
static char *oblivion_web_base_url() { return (char *)(uintptr_t)oblivion_web_base_url_js(); }

namespace oblivion::web {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Block cache: 256 KB blocks, least recently used out, 256 MB in all (about the game's working set of a cell; the
// browser's own HTTP cache sits behind it). Reads of a megabyte or more go straight to the network.

constexpr uint64_t kBlock = 256 * 1024;
constexpr size_t kCacheBlocks = 1024;
constexpr size_t kDirectRead = 1024 * 1024;

struct BlockKey {
  uint32_t file;
  uint64_t block;
  bool operator==(const BlockKey &o) const { return file == o.file && block == o.block; }
};
struct BlockKeyHash {
  size_t operator()(const BlockKey &k) const { return std::hash<uint64_t>()(k.block * 1000003u ^ k.file); }
};
struct Block {
  BlockKey key;
  std::vector<uint8_t> data;
};

std::mutex g_cache_mutex;
std::list<Block> g_lru;  // front: most recent
std::unordered_map<BlockKey, std::list<Block>::iterator, BlockKeyHash> g_index;
std::atomic<uint64_t> g_bytes_fetched{0}, g_requests{0}, g_hits{0};

std::string g_base_url;                                // .../game/
std::unordered_map<std::string, uint64_t> g_sizes;     // path under /game -> size (the manifest)
std::vector<std::string> g_urls;                       // file id -> URL
std::mutex g_files_mutex;

// Copies what the cache has of [block, block + 1) into out; false on a miss.
bool CacheGet(uint32_t file, uint64_t block, std::vector<uint8_t> &out) {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  auto it = g_index.find({file, block});
  if (it == g_index.end()) return false;
  g_lru.splice(g_lru.begin(), g_lru, it->second);
  out = it->second->data;
  return true;
}

void CachePut(uint32_t file, uint64_t block, std::vector<uint8_t> data) {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  if (g_index.count({file, block})) return;
  g_lru.push_front({{file, block}, std::move(data)});
  g_index[{file, block}] = g_lru.begin();
  while (g_lru.size() > kCacheBlocks) {
    g_index.erase(g_lru.back().key);
    g_lru.pop_back();
  }
}

ssize_t Fetch(uint32_t file, uint64_t offset, uint64_t length, uint8_t *dst) {
  std::string url;
  {
    std::lock_guard<std::mutex> lock(g_files_mutex);
    url = g_urls[file];
  }
  const double n = oblivion_web_http_read(url.c_str(), double(offset), double(length), dst);
  if (n < 0) return -EIO;
  g_bytes_fetched += uint64_t(n);
  ++g_requests;
  return ssize_t(n);
}

// ---------------------------------------------------------------------------------------------------------------------
// The WASMFS backend.

class HttpFile : public wasmfs::DataFile {
  uint32_t id_;
  uint64_t size_;

 public:
  HttpFile(mode_t mode, wasmfs::backend_t backend, uint32_t id, uint64_t size)
      : DataFile(mode, backend), id_(id), size_(size) {}

  off_t getSize() override { return off_t(size_); }

 protected:
  int open(wasmfs::oflags_t flags) override { return (flags & O_ACCMODE) == O_RDONLY ? 0 : -EROFS; }
  int close() override { return 0; }
  ssize_t write(const uint8_t *, size_t, off_t) override { return -EROFS; }
  int setSize(off_t) override { return -EROFS; }
  int flush() override { return 0; }

  ssize_t read(uint8_t *buf, size_t len, off_t offset) override {
    if (offset < 0) return -EINVAL;
    if (uint64_t(offset) >= size_) return 0;
    len = size_t(std::min<uint64_t>(len, size_ - uint64_t(offset)));
    if (len >= kDirectRead) {
      size_t done = 0;
      while (done < len) {
        const ssize_t n = Fetch(id_, uint64_t(offset) + done, len - done, buf + done);
        if (n <= 0) return done ? ssize_t(done) : (n < 0 ? n : -EIO);
        done += size_t(n);
      }
      return ssize_t(done);
    }
    size_t done = 0;
    std::vector<uint8_t> block;
    while (done < len) {
      const uint64_t at = uint64_t(offset) + done;
      const uint64_t index = at / kBlock;
      if (CacheGet(id_, index, block)) {
        ++g_hits;
      } else {
        const uint64_t start = index * kBlock;
        const uint64_t want = std::min<uint64_t>(kBlock, size_ - start);
        block.resize(want);
        const ssize_t n = Fetch(id_, start, want, block.data());
        if (n < ssize_t(want)) return done ? ssize_t(done) : -EIO;
        CachePut(id_, index, block);
      }
      const uint64_t in = at - index * kBlock;
      const size_t take = size_t(std::min<uint64_t>(len - done, block.size() - in));
      std::memcpy(buf + done, block.data() + in, take);
      done += take;
    }
    return ssize_t(done);
  }
};

class HttpBackend;

class HttpDirectory : public wasmfs::MemoryDirectory {
  std::string path_;  // under /game, without a leading slash ("" for the root)

 public:
  HttpDirectory(const std::string &path, mode_t mode, wasmfs::backend_t backend)
      : MemoryDirectory(mode, backend), path_(path) {}

  std::string Child(const std::string &name) const { return path_.empty() ? name : path_ + "/" + name; }

  std::shared_ptr<wasmfs::DataFile> insertDataFile(const std::string &name, mode_t mode) override;

  std::shared_ptr<wasmfs::Directory> insertDirectory(const std::string &name, mode_t mode) override {
    auto child = std::make_shared<HttpDirectory>(Child(name), mode, getBackend());
    insertChild(name, child);
    return child;
  }
};

class HttpBackend : public wasmfs::Backend {
 public:
  std::shared_ptr<wasmfs::DataFile> createFile(mode_t) override { return nullptr; }  // only through a directory
  std::shared_ptr<wasmfs::Directory> createDirectory(mode_t mode) override {
    return std::make_shared<HttpDirectory>("", mode, this);
  }
  std::shared_ptr<wasmfs::Symlink> createSymlink(std::string) override { return nullptr; }
};

std::shared_ptr<wasmfs::DataFile> HttpDirectory::insertDataFile(const std::string &name, mode_t mode) {
  const std::string path = Child(name);
  uint64_t size = 0;
  uint32_t id = 0;
  {
    std::lock_guard<std::mutex> lock(g_files_mutex);
    auto it = g_sizes.find(path);
    if (it == g_sizes.end()) return nullptr;  // only the manifest's files exist here
    size = it->second;
    id = uint32_t(g_urls.size());
    std::string url = g_base_url;
    // Percent-encode what a path may hold that a URL may not (the disc has spaces: "Voices NonDialogue").
    for (unsigned char c : path) {
      if (std::isalnum(c) || c == '/' || c == '.' || c == '-' || c == '_') {
        url += char(c);
      } else {
        static const char kHex[] = "0123456789ABCDEF";
        url += '%';
        url += kHex[c >> 4];
        url += kHex[c & 15];
      }
    }
    g_urls.push_back(std::move(url));
  }
  auto child = std::make_shared<HttpFile>(mode, getBackend(), id, size);
  insertChild(name, child);
  return child;
}

// The manifest: one "<size> <path>" line a file (nx/web/serve.js), paths relative to the mount.
bool ParseManifest(const char *text, std::vector<std::pair<std::string, uint64_t>> &files) {
  for (const char *p = text; *p;) {
    const char *end = std::strchr(p, '\n');
    std::string line(p, end ? size_t(end - p) : std::strlen(p));
    p = end ? end + 1 : p + line.size();
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const size_t space = line.find(' ');
    if (space == std::string::npos) return false;
    files.emplace_back(line.substr(space + 1), std::strtoull(line.c_str(), nullptr, 10));
  }
  return !files.empty();
}

}  // namespace

// OPFS's synchronous file handles (what WASMFS's OPFS backend uses on its worker): Chrome 102, Firefox 111, Safari 17.
EM_JS(int, oblivion_web_has_opfs, (), {
  return (typeof navigator !== 'undefined' && navigator.storage && typeof navigator.storage.getDirectory === 'function' &&
          typeof FileSystemFileHandle !== 'undefined' && 'createSyncAccessHandle' in FileSystemFileHandle.prototype) ? 1 : 0;
});

bool MountSaveStorage(const char *mount_point) {
  if (oblivion_web_has_opfs()) {
    ::backend_t opfs = wasmfs_create_opfs_backend();
    if (opfs && wasmfs_create_directory(mount_point, 0777, opfs) == 0) {
      // The browser may clear an origin's storage under pressure unless the page asked to keep it (web/shell.html asks).
      REXLOG_INFO("[web files] {} is the browser's origin private file system (kept across visits)", mount_point);
      return true;
    }
    REXLOG_WARN("[web files] could not put {} on the origin private file system (errno {}): saves last until the "
                "page closes", mount_point, errno);
  } else {
    REXLOG_WARN("[web files] this browser has no origin private file system: saves last until the page closes");
  }
  ::mkdir(mount_point, 0777);
  return false;
}

std::filesystem::path MountGameFiles(const char *mount_point, const char *url_dir) {
  char *base = oblivion_web_base_url();
  g_base_url = std::string(base ? base : "") + url_dir;
  std::free(base);
  if (!g_base_url.empty() && g_base_url.back() != '/') g_base_url += '/';

  char *text = oblivion_web_http_text((g_base_url + ".manifest").c_str());
  std::vector<std::pair<std::string, uint64_t>> files;
  const bool ok = text && ParseManifest(text, files);
  std::free(text);
  if (!ok) {
    REXLOG_ERROR("[web files] no game file manifest at {}.manifest: serve the extracted disc with nx/web/serve.js "
                 "--mount /{}=<folder>", g_base_url, url_dir);
    return {};
  }

  static HttpBackend *backend = nullptr;
  if (!backend) {
    auto owned = std::make_unique<HttpBackend>();
    backend = owned.get();
    wasmfs::wasmFS.addBackend(std::move(owned));
  }
  uint64_t total = 0;
  {
    std::lock_guard<std::mutex> lock(g_files_mutex);
    for (auto &[path, size] : files) {
      g_sizes[path] = size;
      total += size;
    }
  }
  if (wasmfs_create_directory(mount_point, 0755, reinterpret_cast<::backend_t>(backend)) != 0 && errno != EEXIST) {
    REXLOG_ERROR("[web files] could not mount {}: errno {}", mount_point, errno);
    return {};
  }
  const std::string root = mount_point;
  for (auto &[path, size] : files) {
    // Directories first (mkdir of each component), then the file through HttpDirectory::insertDataFile.
    for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
      ::mkdir((root + "/" + path.substr(0, slash)).c_str(), 0755);
    }
    const int fd = ::open((root + "/" + path).c_str(), O_CREAT | O_RDONLY, 0444);
    if (fd < 0) {
      REXLOG_ERROR("[web files] could not create {}/{}: errno {}", root, path, errno);
      continue;
    }
    ::close(fd);
  }
  REXLOG_INFO("[web files] {} files ({} MB) under {} from {}", files.size(), total >> 20, root, g_base_url);
  return root;
}

std::string GameFilesStats() {
  return "web files: " + std::to_string(g_requests.load()) + " requests, " + std::to_string(g_bytes_fetched >> 20) +
         " MB fetched, " + std::to_string(g_hits.load()) + " cache hits";
}

}  // namespace oblivion::web

#endif  // __EMSCRIPTEN__
