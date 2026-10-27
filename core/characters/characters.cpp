#include "rm/characters.h"

namespace rm {

std::string_view CharacterGateName(CharacterGate g) {
    switch (g) {
        case CharacterGate::Selectable: return "selectable";
        case CharacterGate::Unknown: return "unknown";
        case CharacterGate::Disabled: return "disabled";
        case CharacterGate::Locked: return "locked";
    }
    return "unknown";
}

CharacterGate CheckCharacter(const CharacterDef* def, const StringSet& unlocked_characters) {
    if (def == nullptr) return CharacterGate::Unknown;
    if (!def->enabled) return CharacterGate::Disabled;
    if (def->unlock_cost_credits > 0 &&
        unlocked_characters.find(def->id) == unlocked_characters.end()) {
        return CharacterGate::Locked;
    }
    return CharacterGate::Selectable;
}

}  // namespace rm
