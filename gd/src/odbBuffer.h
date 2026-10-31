#pragma once
#include <git2.h>
#include <expected.h>

/**
 * @brief An in-memory ODB backend that queues new blobs and trees and flushes
 *        them as a single pack file at commit time.
 *
 * The backend is attached to a repository's object database and is owned by it;
 * handles returned here are non-owning and remain valid for as long as the
 * repository is open.
 */
struct OdbBufferBackend;

/// @brief Attach a buffering backend to the repository's object database
/// @param repo The repository whose ODB is extended
/// @param enabled Initial state of the buffering path
/// @return A non-owning handle on success, otherwise an Error
Result<OdbBufferBackend *>
attachOdbBuffer(git_repository *repo, bool enabled) noexcept;

/// @brief Buffer a blob directly, bypassing git_odb_write (and its per-object
///        freshen/refresh overhead)
/// @param backend The buffering backend
/// @param data The blob content
/// @param len The content size
/// @param oid Where the computed blob id is stored
/// @return true when the blob was buffered, false when buffering is disabled
bool bufferBlob(OdbBufferBackend *backend, const void *data, size_t len,
                git_oid *oid) noexcept;

/// @brief Flush all buffered objects into a pack file and clear the buffer
/// @param backend The buffering backend
/// @param repo The repository the backend is attached to
/// @return nothing on success, otherwise an Error
Result<void> flushOdbBuffer(OdbBufferBackend *backend,
                            git_repository *repo) noexcept;

/// @brief Enable or disable the buffered write path for this backend
void setOdbBufferEnabled(OdbBufferBackend *backend, bool enabled) noexcept;

/// @brief Number of objects currently buffered (not yet flushed)
size_t bufferedObjectCount(OdbBufferBackend *backend) noexcept;
