#pragma once

#include <utility>

#include <QString>

namespace FileMapping {

enum class ErrorKind {
    None,
    Unavailable,
    Unauthorized,
    Timeout,
    Network,
    NotFound,
    ReadOnly,
    Cancelled,
    Unsupported,
    Internal,
};

struct Error {
    ErrorKind kind = ErrorKind::None;
    QString message;

    bool ok() const { return kind == ErrorKind::None; }

    static Error none() { return {}; }
    static Error make(ErrorKind kind, QString message)
    {
        return { kind, std::move(message) };
    }
};

} // namespace FileMapping
