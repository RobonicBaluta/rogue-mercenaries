#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace rm {

/// Failure categories returned by core operations. Operations never throw for expected
/// misuse (wrong phase, bad index, insufficient funds); they return one of these.
enum class ErrorCode {
    Ok,
    InvalidArgument,
    UnknownId,
    Disabled,
    Locked,
    AlreadyUnlocked,
    NotUnlockable,
    NoActiveRun,
    RunInProgress,
    WrongPhase,
    InvalidIndex,
    AlreadyChosen,
    AlreadyBought,
    InsufficientFunds,
    InvalidAmount,
    AlreadyApplied,
    NotEligible,
    InvalidEncounter,
    ArenaNotComplete,
    NoRerollTokens,
    GrantFailed,
    PersistenceFailed,
    IoError,
    ParseError,
};

std::string_view ErrorCodeName(ErrorCode code);

struct Status {
    ErrorCode code = ErrorCode::Ok;
    std::string message;

    [[nodiscard]] bool ok() const { return code == ErrorCode::Ok; }

    static Status Success() { return {}; }
    static Status Error(ErrorCode c, std::string msg) { return Status{c, std::move(msg)}; }
};

/// Value-or-status. A Result built from a Status never holds a value.
template <class T>
class Result {
public:
    Result(T value) : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
    Result(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const { return status_.ok() && value_.has_value(); }
    [[nodiscard]] const Status& status() const { return status_; }
    [[nodiscard]] ErrorCode code() const { return status_.code; }

    const T& value() const { return *value_; }
    T& value() { return *value_; }
    const T& operator*() const { return *value_; }
    const T* operator->() const { return &*value_; }

private:
    Status status_;
    std::optional<T> value_;
};

}  // namespace rm
