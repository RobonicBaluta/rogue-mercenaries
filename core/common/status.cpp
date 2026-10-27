#include "rm/status.h"

namespace rm {

std::string_view ErrorCodeName(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok: return "ok";
        case ErrorCode::InvalidArgument: return "invalid_argument";
        case ErrorCode::UnknownId: return "unknown_id";
        case ErrorCode::Disabled: return "disabled";
        case ErrorCode::Locked: return "locked";
        case ErrorCode::AlreadyUnlocked: return "already_unlocked";
        case ErrorCode::NotUnlockable: return "not_unlockable";
        case ErrorCode::NoActiveRun: return "no_active_run";
        case ErrorCode::RunInProgress: return "run_in_progress";
        case ErrorCode::WrongPhase: return "wrong_phase";
        case ErrorCode::InvalidIndex: return "invalid_index";
        case ErrorCode::AlreadyChosen: return "already_chosen";
        case ErrorCode::AlreadyBought: return "already_bought";
        case ErrorCode::InsufficientFunds: return "insufficient_funds";
        case ErrorCode::InvalidAmount: return "invalid_amount";
        case ErrorCode::AlreadyApplied: return "already_applied";
        case ErrorCode::NotEligible: return "not_eligible";
        case ErrorCode::InvalidEncounter: return "invalid_encounter";
        case ErrorCode::ArenaNotComplete: return "arena_not_complete";
        case ErrorCode::NoRerollTokens: return "no_reroll_tokens";
        case ErrorCode::GrantFailed: return "grant_failed";
        case ErrorCode::PersistenceFailed: return "persistence_failed";
        case ErrorCode::IoError: return "io_error";
        case ErrorCode::ParseError: return "parse_error";
    }
    return "unknown";
}

}  // namespace rm
