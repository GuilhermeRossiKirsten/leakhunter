/// @file Error.hpp
/// @brief Minimal expected-style result type.
///
/// Exceptions are reserved for genuinely exceptional situations (out of memory,
/// broken invariants). Recoverable failures -- a missing binary, an unreadable
/// trace, a bad flag -- travel as values so callers must handle them.

#pragma once

#include <string>
#include <utility>
#include <variant>

namespace leakhunter {

struct Error {
    std::string message;

    explicit Error(std::string text) : message(std::move(text)) {}
};

template <typename T>
class Result {
public:
    Result(T value) : storage_(std::move(value)) {}       // NOLINT(google-explicit-constructor)
    Result(Error error) : storage_(std::move(error)) {}   // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool hasValue() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] T& value() & { return std::get<0>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

    [[nodiscard]] const Error& error() const& { return std::get<1>(storage_); }
    [[nodiscard]] const std::string& message() const& { return std::get<1>(storage_).message; }

private:
    std::variant<T, Error> storage_;
};

/// Specialisation for operations that either succeed or explain why they did not.
template <>
class Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)), failed_(true) {}  // NOLINT

    [[nodiscard]] bool hasValue() const noexcept { return !failed_; }
    explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] const Error& error() const& { return error_; }
    [[nodiscard]] const std::string& message() const& { return error_.message; }

private:
    Error error_{""};
    bool failed_ = false;
};

using Status = Result<void>;

}  // namespace leakhunter
