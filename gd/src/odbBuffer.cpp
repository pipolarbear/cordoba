#include <odbBuffer.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include <git2/sys/odb_backend.h>
#include <openssl/evp.h>
#include <zlib.h>

namespace {
/// @brief Hash / equality over a raw git_oid, so buffered objects can be keyed
///        by value without an intermediate hex string
struct OidHash {
  std::size_t operator()(const git_oid &oid) const noexcept {
    std::size_t hash{1469598103934665603ull};
    for (std::size_t i = 0; i < GIT_OID_RAWSZ; ++i) {
      hash ^= oid.id[i];
      hash *= 1099511628211ull;
    }
    return hash;
  }
};

struct OidEqual {
  bool operator()(const git_oid &a, const git_oid &b) const noexcept {
    return std::memcmp(a.id, b.id, GIT_OID_RAWSZ) == 0;
  }
};

struct BufferedObject {
  git_object_t type_;
  std::string data_;
};

using BufferedMap =
    std::unordered_map<git_oid, BufferedObject, OidHash, OidEqual>;

/// @brief Append a big-endian 32-bit integer (pack header fields)
void appendBe32(std::string &out, std::uint32_t value) {
  out.push_back(static_cast<char>((value >> 24) & 0xFF));
  out.push_back(static_cast<char>((value >> 16) & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>(value & 0xFF));
}

/// @brief Append a pack object header (type + uncompressed size)
void appendObjectHeader(std::string &out, git_object_t type, std::size_t size) {
  auto byte = static_cast<unsigned char>((static_cast<unsigned>(type) << 4) |
                                         (size & 0x0F));
  size >>= 4;
  while (size) {
    out.push_back(static_cast<char>(byte | 0x80));
    byte = static_cast<unsigned char>(size & 0x7F);
    size >>= 7;
  }
  out.push_back(static_cast<char>(byte));
}

/// @brief Reusable zlib deflater (Z_BEST_SPEED, matching loose-object writes).
///        Reusing the stream via deflateReset avoids a per-object
///        deflateInit/deflateEnd, which dominates flushes of many small objects.
class Deflater {
public:
  Deflater() = default;
  Deflater(const Deflater &) = delete;
  Deflater &operator=(const Deflater &) = delete;
  ~Deflater() {
    if (initialized_)
      deflateEnd(&stream_);
  }

  bool compress(const std::string &in, std::string &out) {
    if (!initialized_) {
      if (deflateInit(&stream_, Z_BEST_SPEED) != Z_OK)
        return false;
      initialized_ = true;
    } else if (deflateReset(&stream_) != Z_OK) {
      return false;
    }

    out.resize(deflateBound(&stream_, static_cast<uLong>(in.size())));
    stream_.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.data()));
    stream_.avail_in = static_cast<uInt>(in.size());
    stream_.next_out = reinterpret_cast<Bytef *>(out.data());
    stream_.avail_out = static_cast<uInt>(out.size());

    const uLong before = stream_.total_out;
    int rc = deflate(&stream_, Z_FINISH);
    out.resize(rc == Z_STREAM_END ? stream_.total_out - before : 0);
    return rc == Z_STREAM_END;
  }

private:
  z_stream stream_{};
  bool initialized_{false};
};
} // namespace

struct OdbBufferBackend {
  git_odb_backend parent{};
  std::mutex mutex;
  BufferedMap active;
  BufferedMap flushing;
  std::atomic<bool> enabled{false};
};

namespace {
OdbBufferBackend *self(git_odb_backend *backend) noexcept {
  return reinterpret_cast<OdbBufferBackend *>(backend);
}

/// @brief Locate a buffered object in either the active or flushing set
///        Caller must hold the backend mutex
BufferedObject *findBuffered(OdbBufferBackend &backend,
                             const git_oid &oid) noexcept {
  if (auto itr = backend.active.find(oid); itr != backend.active.end())
    return &itr->second;
  if (auto itr = backend.flushing.find(oid); itr != backend.flushing.end())
    return &itr->second;
  return nullptr;
}

int implWrite(git_odb_backend *backend, const git_oid *oid, const void *data,
              size_t len, git_object_t type) {
  auto *buffer = self(backend);

  // Only blobs and trees are buffered; commits/tags fall through to the loose
  // backend, preserving git_commit_create's reference update semantics.
  if (!buffer->enabled.load(std::memory_order_relaxed))
    return GIT_PASSTHROUGH;
  if (type != GIT_OBJECT_BLOB && type != GIT_OBJECT_TREE)
    return GIT_PASSTHROUGH;

  std::lock_guard<std::mutex> lock(buffer->mutex);
  if (!buffer->active.contains(*oid))
    buffer->active.emplace(
        *oid, BufferedObject{type,
                             std::string(static_cast<const char *>(data), len)});
  return 0;
}

int implRead(void **buffer_p, size_t *len_p, git_object_t *type_p,
             git_odb_backend *backend, const git_oid *oid) {
  auto *buffer = self(backend);

  std::lock_guard<std::mutex> lock(buffer->mutex);
  BufferedObject *obj = findBuffered(*buffer, *oid);
  if (!obj)
    return GIT_PASSTHROUGH;

  *len_p = obj->data_.size();
  *type_p = obj->type_;
  *buffer_p = git_odb_backend_data_alloc(backend, obj->data_.size());
  if (!*buffer_p)
    return -1;
  std::memcpy(*buffer_p, obj->data_.data(), obj->data_.size());
  return 0;
}

int implReadHeader(size_t *len_p, git_object_t *type_p, git_odb_backend *backend,
                   const git_oid *oid) {
  auto *buffer = self(backend);

  std::lock_guard<std::mutex> lock(buffer->mutex);
  BufferedObject *obj = findBuffered(*buffer, *oid);
  if (!obj)
    return GIT_PASSTHROUGH;

  *len_p = obj->data_.size();
  *type_p = obj->type_;
  return 0;
}

int implExists(git_odb_backend *backend, const git_oid *oid) {
  auto *buffer = self(backend);

  std::lock_guard<std::mutex> lock(buffer->mutex);
  return findBuffered(*buffer, *oid) ? 1 : 0;
}

/// @brief Streaming writes (e.g. git_blob_create_from_buffer) must not be
///        wrapped around this backend's `write`: pass through so the loose
///        backend handles them. Following the `write` path would turn a
///        "not buffered" result into a hard error.
int implWritestream(git_odb_stream **, git_odb_backend *, git_object_size_t,
                    git_object_t) {
  return GIT_PASSTHROUGH;
}

void implFree(git_odb_backend *backend) { delete self(backend); }
} // namespace

Result<OdbBufferBackend *> attachOdbBuffer(git_repository *repo,
                                           bool enabled) noexcept {
  if (!repo)
    return gd_unexpected(gd::ErrorType::MissingRepository,
                         "No repository to buffer");

  git_odb *odb = nullptr;
  if (git_repository_odb(&odb, repo) != 0)
    return gd_unexpected();

  auto *buffer = new (std::nothrow) OdbBufferBackend();
  if (!buffer) {
    git_odb_free(odb);
    return gd_unexpected();
  }

  buffer->parent.version = GIT_ODB_BACKEND_VERSION;
  buffer->parent.write = &implWrite;
  buffer->parent.writestream = &implWritestream;
  buffer->parent.read = &implRead;
  buffer->parent.read_header = &implReadHeader;
  buffer->parent.exists = &implExists;
  buffer->parent.free = &implFree;
  buffer->enabled.store(enabled);

  // Highest priority: writes are offered here first, and fall through to the
  // loose backend when not buffered.
  if (git_odb_add_backend(odb, &buffer->parent, 999) != 0) {
    delete buffer;
    git_odb_free(odb);
    return gd_unexpected();
  }

  git_odb_free(odb); // the ODB owns the backend; the repo keeps it alive
  return buffer;
}

bool bufferBlob(OdbBufferBackend *backend, const void *data, size_t len,
                git_oid *oid) noexcept {
  if (!backend || !backend->enabled.load(std::memory_order_relaxed))
    return false;

  if (git_odb_hash(oid, data, len, GIT_OBJECT_BLOB) != 0)
    return false;

  std::lock_guard<std::mutex> lock(backend->mutex);
  if (!backend->active.contains(*oid))
    backend->active.emplace(
        *oid, BufferedObject{GIT_OBJECT_BLOB,
                             std::string(static_cast<const char *>(data), len)});
  return true;
}

Result<void> flushOdbBuffer(OdbBufferBackend *backend,
                            git_repository *repo) noexcept {
  if (!backend || !repo)
    return Result<void>();

  std::vector<git_oid> oids;
  {
    std::lock_guard<std::mutex> lock(backend->mutex);
    if (backend->active.empty())
      return Result<void>();

    for (auto &[oid, obj] : backend->active)
      backend->flushing.insert_or_assign(oid, std::move(obj));
    backend->active.clear();

    oids.reserve(backend->flushing.size());
    for (const auto &[oid, _] : backend->flushing)
      oids.push_back(oid);
  }

  auto restore = [&backend]() noexcept {
    std::lock_guard<std::mutex> lock(backend->mutex);
    for (auto &[oid, obj] : backend->flushing)
      backend->active.insert_or_assign(oid, std::move(obj));
    backend->flushing.clear();
  };

  // Build a single no-delta pack and stream it into the ODB. Document content
  // does not delta well, and skipping libgit2's packbuilder avoids both its
  // delta search and its level-6 compression (loose writes use Z_BEST_SPEED).
  // The pack is streamed with a running SHA-1 so it is never held whole in
  // memory.
  git_odb *odb = nullptr;
  if (git_repository_odb(&odb, repo) != 0) {
    restore();
    return gd_unexpected();
  }

  git_odb_writepack *writepack = nullptr;
  git_indexer_progress stats{};
  int error = git_odb_write_pack(&writepack, odb, nullptr, nullptr);
  if (error != 0) {
    git_odb_free(odb);
    restore();
    return gd_unexpected();
  }

  EVP_MD_CTX *digestCtx = EVP_MD_CTX_new();
  if (!digestCtx || EVP_DigestInit_ex(digestCtx, EVP_sha1(), nullptr) != 1) {
    EVP_MD_CTX_free(digestCtx);
    writepack->free(writepack);
    git_odb_free(odb);
    restore();
    return gd_unexpected();
  }

  auto emit = [&](const char *data, std::size_t len) -> int {
    EVP_DigestUpdate(digestCtx, data, len);
    return writepack->append(writepack, data, len, &stats);
  };

  constexpr std::size_t FlushChunkSize = 64 * 1024;
  std::string chunk;
  chunk.reserve(FlushChunkSize);
  chunk += "PACK";
  appendBe32(chunk, 2); // pack version
  appendBe32(chunk, static_cast<std::uint32_t>(oids.size()));

  auto flushChunk = [&]() -> int {
    if (chunk.empty())
      return 0;
    int rc = emit(chunk.data(), chunk.size());
    chunk.clear();
    return rc;
  };

  Deflater deflater;
  std::string compressed;
  for (const auto &oid : oids) {
    if (error != 0)
      break;

    const BufferedObject *obj = nullptr;
    {
      std::lock_guard<std::mutex> lock(backend->mutex);
      obj = findBuffered(*backend, oid);
      if (!obj) {
        error = -1;
        break;
      }
    }

    // `flushing` is stable for the duration of the flush (new writes go to
    // `active`, and flushes are serialized), so the object can be read without
    // holding the lock while it is compressed.
    appendObjectHeader(chunk, obj->type_, obj->data_.size());
    if (!deflater.compress(obj->data_, compressed)) {
      error = -1;
      break;
    }
    chunk += compressed;

    if (chunk.size() >= FlushChunkSize)
      error = flushChunk();
  }
  if (error == 0)
    error = flushChunk();

  if (error == 0) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;
    if (EVP_DigestFinal_ex(digestCtx, digest, &digestLen) != 1)
      error = -1;
    else
      // The trailer is not part of the hashed content.
      error = writepack->append(writepack,
                                reinterpret_cast<const char *>(digest),
                                digestLen, &stats);
  }
  if (error == 0)
    error = writepack->commit(writepack, &stats);

  EVP_MD_CTX_free(digestCtx);
  writepack->free(writepack);
  if (error == 0)
    git_odb_refresh(odb);
  git_odb_free(odb);

  if (error != 0) {
    restore();
    return gd_unexpected();
  }

  {
    std::lock_guard<std::mutex> lock(backend->mutex);
    backend->flushing.clear();
  }
  return Result<void>();
}

void setOdbBufferEnabled(OdbBufferBackend *backend, bool enabled) noexcept {
  if (backend)
    backend->enabled.store(enabled);
}

size_t bufferedObjectCount(OdbBufferBackend *backend) noexcept {
  if (!backend)
    return 0;
  std::lock_guard<std::mutex> lock(backend->mutex);
  return backend->active.size() + backend->flushing.size();
}
