#pragma once

#include <optional>
#include <string>
#include <vector>

#include "rm/director.h"
#include "rm/rng.h"
#include "rm/routes.h"

namespace rm {

class Content;

/// Run-planned rosters (ROADMAP M3.3, economy run_rosters): gives the nodes of `map` their
/// guests, classes of other maps that join the node map's own enemies. Layers before
/// run_rosters.from_arena and boss arenas get none. A node gets up to normal_guests normal
/// classes and, when its threat level lets specials in, up to special_guests specials, never a
/// class its map already has, never past max_classes classes in all, each at or under the node's
/// threat level (when threat levels are on). Classes the paths into the node have not fielded
/// come first (the arenas on the way, their own enemies and their guests; with two paths in, the
/// fewer paths the better), then classes not yet given to another node of the layer; among
/// those, harder classes are likelier. Does nothing when run_rosters is disabled.
void PlanRunRosters(const Content& content, Rng& rng, RunMap& map);

/// The guest's roster entry: its home map's entry for the class (economy run_rosters.guests),
/// with `home` set; empty for a class that does not travel.
std::optional<MapEnemy> GuestEntry(const Content& content, const std::string& unit_class);

/// The names of the guests' home maps, each once, in guest order: "Ship Deck and Village".
std::string GuestHomes(const Content& content, const std::vector<std::string>& guests);

/// `map` with `guests` added to its roster (classes it already has, or that do not travel, are
/// skipped). Normal guests share run_rosters.normal_share of the normal picks between them and
/// special guests special_share of the special picks; on a map without specials of its own a
/// special guest keeps its home weight.
MapDef WithGuests(const Content& content, const MapDef& map, const std::vector<std::string>& guests);

}  // namespace rm
