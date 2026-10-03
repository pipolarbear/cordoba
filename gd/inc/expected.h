#pragma once

#include <expected>
#include <git2.h>
#include <err.h>

template <typename T, typename E>
using expected = std::expected<T,E>;

template <typename T>
using Result = expected<T, gd::Error>;


  namespace
{
  /// @brief creates unexpected results with user data
  /// @param type type of the unexpected
  /// @param msg description
  /// @param loc source location
  /// @return the newly created unexpected 
  std::unexpected<gd::Error>
  gd_unexpected(gd::ErrorType type, const std::string&& msg, std::source_location loc = std::source_location::current()) {
    return std::unexpected<gd::Error>(gd::Error(type, std::move(msg), loc));
  }

  /// @brief returns an unexpected from result
  /// @param result The result containing an error
  /// @return the moved unexpected value 
  template <typename T>
  std::unexpected<gd::Error>
  gd_unexpected(Result<T>&& result) {
    return std::unexpected<gd::Error>(std::move(result.error()));
  }

  /// @brief returns an unexpected from an error
  /// @param err The error to wrap
  /// @return the unexpected value 
  inline std::unexpected<gd::Error>
  gd_unexpected(const gd::Error& err) {
    return std::unexpected<gd::Error>(err);
  }

  /// @brief creates an unexpected with git error
  /// @param loc source location
  /// @return the newly created unexpected 
  std::unexpected<gd::Error>
  gd_unexpected(std::source_location loc = std::source_location::current()){
    return std::unexpected<gd::Error>(gd::Error(gd::ErrorType::GitError, git_error_last(), loc));
  }
}

/// @brief Unwraps the value of a Result for GD_TRY.
/// @return The contained value, by value (the value of a GNU statement
///         expression must not refer to a local that is about to be destroyed).
///         The Result<void> overload yields void.
template <typename T>
T gd_try_unwrap(Result<T>&& result) noexcept {
  return std::move(*result);
}

inline void gd_try_unwrap(Result<void>&&) noexcept {}

/**
 * Rust-like error propagation operator (`?`).
 *
 * GD_TRY(expr) unwraps `expr` when it holds a value, otherwise returns its
 * error to the caller (moving the error, no copy). On GCC/Clang it is an
 * expression (GNU statement expression) and yields the value; the portable
 * fallback is a statement and yields nothing.
 *
 * GD_TRY_ASSIGN(var, expr) unwraps `expr` and binds `var` to the value.
 *
 * Both require the enclosing function to return a Result<...>; on the value
 * path no extra object is created (GCC/Clang), on the error path only the
 * returned error is moved.
 */
#if defined(__GNUC__)
  #define GD_TRY(expr) \
    ({ auto _gd_r = (expr); \
       if (!_gd_r) return gd_unexpected(std::move(_gd_r)); \
       gd_try_unwrap(std::move(_gd_r)); })
  #define GD_TRY_ASSIGN(var, expr) auto var = GD_TRY(expr)
#else
  #define GD_TRY(expr) \
    do { auto _gd_r = (expr); if (!_gd_r) return gd_unexpected(std::move(_gd_r)); } while (0)
  #define GD_TRY_ASSIGN(var, expr) \
    auto _gd_r = (expr); \
    if (!_gd_r) return gd_unexpected(std::move(_gd_r)); \
    auto var = gd_try_unwrap(std::move(_gd_r)); \
    static_assert(true)
#endif
