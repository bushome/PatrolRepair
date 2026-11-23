#pragma once

// Pure logic for PatrolRepair: config parsing, the per-manager patrol report and the clean plan.
//
// No dependency on the game or on server API types (only on the nlohmann json.hpp that ships with
// AsaApi), so it compiles and unit-tests on any machine. See tests/logic_tests.cpp.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"

namespace PatrolRepair
{
	inline std::string ToLower(std::string_view text)
	{
		std::string out(text);
		for (char& c : out)
			c = static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
		return out;
	}

	inline bool ContainsNoCase(std::string_view text, std::string_view part)
	{
		return !part.empty() && ToLower(text).find(ToLower(part)) != std::string::npos;
	}

	struct Settings
	{
		bool logging = true;             // false: nothing is written to the API log or console
		int scan_actors_per_tick = 5000;
		std::string rider_family = "Thrall_Character_BP_Base_C";             // parent class of every patrol rider
		std::string mount_path = "/Thrall/Characters/RideableCreatures/";    // path part of every patrol mount
		std::string mount_suffix = "_Thrall_C";                              // or class name ending of a patrol mount
		double still_distance = 300.0;   // moved less than this between two scans = standing still
		int report_managers = 10;        // managers listed in the report, most unmounted riders first
		int removals_per_tick = 10;      // stray riders removed per second by a clean
		bool remove_unlinked = true;     // a clean removes riders and empty mounts no manager is linked to
		bool remove_unpaired = true;     // a clean removes a manager's riders on foot and mounts nobody rides
		int auto_clean_minutes = 0;      // 0 = off; otherwise scan and clean on this interval
		int clean_after_boot_minutes = 10; // 0 = off; otherwise scan and clean once this long after load
		bool reset_negative_count = true;  // after each scan, a patrol manager's negative linked count is set to 0
		bool restore_spawn_entries = true; // a patrol manager's empty NPCSpawnEntriesGroup* are set from its class default
		bool dissolve_orphan_groups = true; // after each scan, saved groups with nothing left of them are dissolved
		int scan_after_boot_minutes = 3;   // report-only scan this long after load (0: off), so orphan groups go early
		std::vector<std::string> warnings;
	};

	namespace detail
	{
		inline int ReadInt(const nlohmann::json& object, const char* key, int fallback, int minimum,
			std::vector<std::string>& warnings)
		{
			if (!object.is_object() || !object.contains(key))
				return fallback;
			const nlohmann::json& value = object[key];
			if (!value.is_number_integer() || value.get<int>() < minimum)
			{
				warnings.push_back(std::string(key) + " must be a whole number of at least " + std::to_string(minimum) +
					"; using " + std::to_string(fallback));
				return fallback;
			}
			return value.get<int>();
		}

		inline std::string ReadString(const nlohmann::json& object, const char* key, const std::string& fallback,
			std::vector<std::string>& warnings)
		{
			if (!object.is_object() || !object.contains(key))
				return fallback;
			const nlohmann::json& value = object[key];
			if (!value.is_string() || value.get<std::string>().empty())
			{
				warnings.push_back(std::string(key) + " must be a non-empty string; using \"" + fallback + "\"");
				return fallback;
			}
			return value.get<std::string>();
		}

		inline bool ReadBool(const nlohmann::json& object, const char* key, bool fallback,
			std::vector<std::string>& warnings)
		{
			if (!object.is_object() || !object.contains(key))
				return fallback;
			const nlohmann::json& value = object[key];
			if (!value.is_boolean())
			{
				warnings.push_back(std::string(key) + " must be true or false; using " + (fallback ? "true" : "false"));
				return fallback;
			}
			return value.get<bool>();
		}
	}

	inline Settings ParseSettings(const nlohmann::json& root)
	{
		if (!root.is_object())
			throw std::runtime_error("config.json must be a JSON object");
		Settings settings;
		const nlohmann::json general = root.value("General", nlohmann::json::object());
		settings.logging = detail::ReadBool(general, "Logging", settings.logging, settings.warnings);
		settings.scan_actors_per_tick = detail::ReadInt(general, "ScanActorsPerTick", settings.scan_actors_per_tick, 100, settings.warnings);
		settings.still_distance = detail::ReadInt(general, "StillDistance", static_cast<int>(settings.still_distance), 0, settings.warnings);
		settings.report_managers = detail::ReadInt(general, "ReportManagers", settings.report_managers, 1, settings.warnings);
		const nlohmann::json patrols = root.value("Patrols", nlohmann::json::object());
		settings.rider_family = detail::ReadString(patrols, "RiderFamily", settings.rider_family, settings.warnings);
		settings.mount_path = detail::ReadString(patrols, "MountPathContains", settings.mount_path, settings.warnings);
		settings.mount_suffix = detail::ReadString(patrols, "MountClassEndsWith", settings.mount_suffix, settings.warnings);
		const nlohmann::json clean = root.value("Clean", nlohmann::json::object());
		settings.removals_per_tick = detail::ReadInt(clean, "RemovalsPerTick", settings.removals_per_tick, 1, settings.warnings);
		settings.remove_unlinked = detail::ReadBool(clean, "RemoveUnlinked", settings.remove_unlinked, settings.warnings);
		settings.remove_unpaired = detail::ReadBool(clean, "RemoveUnpaired", settings.remove_unpaired, settings.warnings);
		settings.clean_after_boot_minutes = detail::ReadInt(clean, "CleanAfterBootMinutes", settings.clean_after_boot_minutes, 0, settings.warnings);
		settings.auto_clean_minutes = detail::ReadInt(clean, "AutoCleanMinutes", settings.auto_clean_minutes, 0, settings.warnings);
		settings.reset_negative_count = detail::ReadBool(clean, "ResetNegativeLinkedCount", settings.reset_negative_count, settings.warnings);
		const nlohmann::json spawning = root.value("Spawning", nlohmann::json::object());
		settings.restore_spawn_entries = detail::ReadBool(spawning, "RestoreSpawnEntries", settings.restore_spawn_entries, settings.warnings);
		settings.dissolve_orphan_groups = detail::ReadBool(spawning, "DissolveOrphanGroups", settings.dissolve_orphan_groups, settings.warnings);
		settings.scan_after_boot_minutes = detail::ReadInt(spawning, "ScanAfterBootMinutes", settings.scan_after_boot_minutes, 0, settings.warnings);
		return settings;
	}

	// ------------------------------------------------------------------------------------------
	// Report
	// ------------------------------------------------------------------------------------------

	enum class Role
	{
		Rider,
		Mount,
	};

	// One patrol creature seen by a scan.
	struct Sighting
	{
		uint64_t creature = 0;    // identifies the creature within one scan
		double x = 0, y = 0, z = 0;
		uint64_t manager = 0;     // 0 = no zone manager link
		Role role = Role::Rider;
		bool mission = false;     // belongs to an outpost or other mission
		bool mounted = false;     // rider: sitting on a mount; mount: carrying a rider
		bool known = false;       // seen by the previous scan too
		double moved = 0;         // distance since the previous scan (when known)
	};

	// One entry of a patrol manager's SavedGroups (Struct_PatrolGroupSavedData), plus what the scan found.
	struct SavedGroup
	{
		int id = 0;
		int size = 0;         // 0 small, 1 medium, 2 large
		int members = 0;      // NumMembers
		int latent = 0;       // LatentSpawnCount: members hibernated, to be spawned again
		int live_linked = 0;  // creatures carrying this PatrolGroupID and linked to this manager
		int live_any = 0;     // creatures carrying this PatrolGroupID, linked anywhere or not at all
	};

	// A group object (PatrolGroupManager_C) in a patrol manager's PatrolGroups map. The manager's
	// ValidateGroup (one group every 2.5 s) dissolves a group once no member has a target, its navigation
	// time is at least kStuckDestroyAfterSeconds old (or never set) and AreAllMembersLoaded: the group's
	// own LatentSpawnCount is 0 and its valid, awake GroupMembers are at least its own NumMembers. The
	// manager's OnGroupDissolved then counts the group down only when its id is in SavedGroups with a
	// LatentSpawnCount of 0. (From the 97.16 DevKit Blueprints.)
	struct GroupObject
	{
		int key = 0;            // PatrolGroups map key
		int id = 0;             // GroupID
		int size = 0;           // GroupSize: 0 small, 1 medium, 2 large
		int num_members = 0;    // NumMembers: the group's own copy
		int latent = 0;         // LatentSpawnCount: the group's own copy
		int listed = 0;         // GroupMembers.Num()
		int valid = 0;          // entries of GroupMembers that point at a creature
		int awake = 0;          // valid and not in stasis
		bool nav_read = false;  // the fields below were read
		bool nav_never = false; // LastTimeUpdatedNavigationTargetPoint is 0
		double nav_age = 0;     // seconds since LastTimeUpdatedNavigationTargetPoint
		bool path = false;      // bDidGeneratePath
		int saved_index = -1;   // index of the SavedGroups entry with the same id; -1 none
	};

	constexpr double kStuckDestroyAfterSeconds = 180;  // StuckDestroyAfterTime of NPCZoneManager_Patrols

	// One patrol spawn point (SSP_PatrolSpawn_*) of a size the manager is below its cap on, with the tests
	// the Blueprint's FindSpawnPoint makes before it uses the point. The game's own distance checks are
	// called; the dino test is DinoObstructionCheck: no dino pawn within ObstructionRadius(size).
	struct SpawnPointCheck
	{
		int size = 0;
		std::string name;
		double x = 0, y = 0, z = 0;
		bool remaining = false;   // still in RemainingSpawnPoints<Size>: only those are tried
		bool players = true;      // MinimumPlayerDistanceCheck passed
		bool tames = true;        // MinimumTamedDinoDistanceCheck passed
		bool structures = true;   // MinimumStructureDistanceCheck passed
		int dinos = 0;            // dino pawns within ObstructionRadius(size)
		std::string nearest;      // class of the closest of them
	};

	inline double ObstructionRadius(int size)
	{
		return size == 2 ? 1200 : size == 1 ? 800 : 500;
	}

	inline bool SpawnPointUsable(const SpawnPointCheck& check)
	{
		return check.players && check.tames && check.structures && check.dinos == 0;
	}

	struct ManagerInfo
	{
		std::string name;
		bool has_count = false;
		double linked_count = 0;  // DirectLinkDinoCount: what the manager believes it has out
		int linked_list = 0;      // DirectLinkedDinos.Num(): creatures it still holds a link to
		int save_version = 0;     // SaveForceResetPatrolsVersion (stored in the save)
		int load_version = 0;     // LoadForceResetPatrolsVersion (what this game build expects)
		bool has_zone = false;    // the fields below were read from the manager actor
		double x = 0, y = 0, z = 0;
		bool enabled = false;     // bEnabled
		int spawned = 0;          // NumNPCSpawned
		int min_desired = 0;      // MinDesiredNumberOfNPC
		int max_npc = 0;          // MaxNumberOfNPC
		int entries = 0;          // NPCSpawnEntries.Num()
		// Group bookkeeping of the patrol manager's Blueprint (NPCZoneManager_Patrols_C). Its tick sends out
		// a group of a size only while that size's count is below its cap. The counts are not saved: at
		// BeginPlay they are rebuilt from SavedGroups (which is), and only a group dissolving lowers them.
		bool has_groups = false;          // the fields below were read
		std::string groups_error;         // why the groups or saved groups could not be read
		int groups[3] = {};               // NumSmallGroups, NumMediumGroups, NumLargeGroups
		int max_groups[3] = {};           // MaxPatrolGroupsSmall, ...Medium, ...Large
		int live_groups = -1;             // PatrolGroups.Num(): group objects in memory; -1 when not read
		bool validated = false;           // bValidatedNumGroups: counts rebuilt from SavedGroups this load
		bool manual = true;               // bUsesManualSpawning: the Blueprint tick does the spawning
		double initial_delay = 0;         // InitialDelay: seconds left before the tick starts spawning
		bool saved_read = false;          // saved was read
		std::vector<SavedGroup> saved;    // SavedGroups
		bool objects_read = false;        // objects was read
		std::string objects_error;        // why the group objects could not be read
		std::vector<GroupObject> objects; // PatrolGroups
		// Spawning: counts of SpawnPoints<Size> / RemainingSpawnPoints<Size>, and for sizes below cap the
		// checks of each spawn point.
		bool spawn_read = false;
		std::string spawn_error;
		int points[3] = {};               // SpawnPointsSmall, ...Medium, ...Large
		int remaining[3] = {};            // RemainingSpawnPointsSmall, ...
		bool trying_full = false;         // bTryingFullIncrease
		double since_try = -1;            // seconds since LastManuallySpawnedTime; -1 not read
		double try_interval = 0;          // MinimumManualSpawnInterval
		std::vector<SpawnPointCheck> spawn_checks;
		// Patrol points (PatrolPoints of the manager) and their connections. FindNearestPatrolPoint only
		// takes a point with a connection whose PathSize >= the group size and OccupiedByGroupID <= 0; with
		// none, DoSpawnPatrolGroup sends nothing out.
		std::vector<int> group_keys;      // PatrolGroups keys (read even when the group objects are not)
		bool patrol_read = false;
		std::string patrol_error;
		int patrol_points = 0;            // PatrolPoints.Num()
		bool patrol_initialized = false;  // bInitilizedPatrolPoints (sic)
		int connections = 0;
		int free_connections[3] = {};     // free connections usable by a small / medium / large group
		int points_with_free[3] = {};     // points with at least one of those
		std::vector<std::pair<int, int>> occupied;  // OccupiedByGroupID -> connections, most first
		// NPCSpawnEntriesGroupSmall/Medium/Large: the spawn entries container class the Blueprint hands the
		// native SpawnPatrolGroup for a group of each size, and the class default of each. "none" = empty;
		// with an empty one the group is sent out with no members.
		bool containers_read = false;
		std::string containers_error;
		std::string containers[3];
		std::string default_containers[3];
	};

	inline std::string GroupSizeName(int size)
	{
		switch (size)
		{
		case 0: return "S";
		case 1: return "M";
		case 2: return "L";
		default: return "size " + std::to_string(size);
		}
	}

	// [groups small 8/8, medium 4/4, large 0/0 (at cap), group objects 0, saved S/M/L 8/4/0 (12 with no live
	// member), validated, delay 0 s]. "at cap": every size is at its cap, so the manager sends out nothing.
	inline std::string FormatGroupSummary(const ManagerInfo& info)
	{
		std::ostringstream out;
		static const char* const names[3] = { "small", "medium", "large" };
		bool at_cap = true;
		out << "[groups";
		for (int i = 0; i < 3; ++i)
		{
			out << (i == 0 ? " " : ", ") << names[i] << " " << info.groups[i] << "/" << info.max_groups[i];
			if (info.groups[i] < info.max_groups[i])
				at_cap = false;
		}
		if (at_cap)
			out << " (at cap)";
		out << ", group objects ";
		if (info.live_groups < 0)
			out << "?";
		else
			out << info.live_groups;
		if (info.saved_read)
		{
			int per_size[3] = {};
			int without_live = 0;
			for (const SavedGroup& group : info.saved)
			{
				if (group.size >= 0 && group.size < 3)
					++per_size[group.size];
				if (group.live_any == 0)
					++without_live;
			}
			out << ", saved S/M/L " << per_size[0] << "/" << per_size[1] << "/" << per_size[2];
			if (without_live > 0)
				out << " (" << without_live << " with no live member)";
		}
		else
		{
			out << ", saved groups not read: " << (info.groups_error.empty() ? std::string("unknown") : info.groups_error);
		}
		if (info.spawn_read)
			out << ", spawn points S/M/L " << info.points[0] << "/" << info.points[1] << "/" << info.points[2];
		out << (info.validated ? ", validated" : ", not validated");
		if (!info.manual)
			out << ", not manual";
		char delay[32];
		// The tick stops counting once the delay is used up, so it is usually a little below 0.
		std::snprintf(delay, sizeof(delay), "%.0f", (std::max)(0.0, info.initial_delay));
		out << ", delay " << delay << " s]";
		return out.str();
	}

	// Log section: per patrol manager with saved groups, each one as #id size members/latent and the live
	// creatures carrying its id (linked to this manager / anywhere).
	inline std::string FormatSavedGroups(const std::map<uint64_t, ManagerInfo>& info)
	{
		std::vector<const ManagerInfo*> list;
		for (const auto& [key, manager] : info)
			if (manager.saved_read && !manager.saved.empty())
				list.push_back(&manager);
		std::sort(list.begin(), list.end(), [](const ManagerInfo* a, const ManagerInfo* b) { return a->name < b->name; });

		std::ostringstream out;
		out << "Saved patrol groups (#id size members/latent, live linked/anywhere):";
		if (list.empty())
			out << " none";
		for (const ManagerInfo* manager : list)
		{
			out << "\n  " << manager->name << ":";
			for (const SavedGroup& group : manager->saved)
				out << " #" << group.id << " " << GroupSizeName(group.size) << " " << group.members << "/" << group.latent
					<< " live " << group.live_linked << "/" << group.live_any << ";";
		}
		return out.str();
	}

	// Saved groups that hold one of the manager's size slots but can never dissolve by themselves: no
	// creature anywhere carries the id, none is waiting to be spawned again (latent 0), and the manager has
	// no group object for it (ValidateGroup only looks at group objects; members that load link back and
	// make one). Such entries are left when a save is made while the game counts groups it spawned nothing
	// for (on Lost Colony with no player online). On load they set the counts to the cap, so the manager
	// never sends out a group again. Only once the counts were rebuilt this load (validated), and only when
	// the group objects' keys were read.
	inline std::vector<int> OrphanGroups(const ManagerInfo& info)
	{
		std::vector<int> ids;
		if (!info.has_groups || !info.saved_read || !info.validated || info.live_groups < 0 ||
			info.group_keys.size() != static_cast<size_t>(info.live_groups))
			return ids;
		for (const SavedGroup& group : info.saved)
		{
			if (group.id <= 0 || group.latent > 0 || group.live_any > 0)
				continue;
			if (std::find(info.group_keys.begin(), info.group_keys.end(), group.id) != info.group_keys.end())
				continue;
			if (std::find(ids.begin(), ids.end(), group.id) == ids.end())
				ids.push_back(group.id);
		}
		return ids;
	}

	// What keeps the manager's ValidateGroup from dissolving this group, or "nothing" when it should.
	inline std::string GroupBlocker(const GroupObject& group, const ManagerInfo& manager)
	{
		std::vector<std::string> reasons;
		if (group.latent > 0)
			reasons.push_back("latent " + std::to_string(group.latent));
		if (group.awake < group.num_members)
			reasons.push_back("awake " + std::to_string(group.awake) + " < NumMembers " + std::to_string(group.num_members));
		if (group.nav_read && !group.nav_never && group.nav_age < kStuckDestroyAfterSeconds)
		{
			char age[32];
			std::snprintf(age, sizeof(age), "%.0f", group.nav_age);
			reasons.push_back(std::string("nav updated ") + age + " s ago");
		}
		if (manager.saved_read)
		{
			if (group.saved_index < 0)
				reasons.push_back("not in SavedGroups, so dissolving would not count it down");
			else if (manager.saved[static_cast<size_t>(group.saved_index)].latent > 0)
				reasons.push_back("saved latent " + std::to_string(manager.saved[static_cast<size_t>(group.saved_index)].latent));
		}
		if (reasons.empty())
			return group.awake > 0 ? "nothing unless a member has a target" : "nothing (should dissolve)";
		std::string text;
		for (size_t i = 0; i < reasons.size(); ++i)
			text += (i == 0 ? "" : ", ") + reasons[i];
		return text;
	}

	// Log section: each patrol manager's group objects as #id size, NumMembers/latent, members
	// listed/valid/awake, navigation age, and what keeps ValidateGroup from dissolving the group.
	inline std::string FormatGroupObjects(const std::map<uint64_t, ManagerInfo>& info)
	{
		std::vector<const ManagerInfo*> list;
		for (const auto& [key, manager] : info)
			if (manager.objects_read && !manager.objects.empty())
				list.push_back(&manager);
		std::sort(list.begin(), list.end(), [](const ManagerInfo* a, const ManagerInfo* b) { return a->name < b->name; });

		std::ostringstream out;
		out << "Patrol group objects (#id size NumMembers/latent, members listed/valid/awake, nav; blocked by):";
		if (list.empty())
			out << " none";
		for (const ManagerInfo* manager : list)
		{
			out << "\n  " << manager->name << ":";
			for (const GroupObject& group : manager->objects)
			{
				out << " #" << group.id;
				if (group.key != group.id)
					out << " (key " << group.key << ")";
				out << " " << GroupSizeName(group.size) << " " << group.num_members << "/" << group.latent << " members "
					<< group.listed << "/" << group.valid << "/" << group.awake << " nav ";
				if (!group.nav_read)
					out << "?";
				else if (group.nav_never)
					out << "never";
				else
				{
					char age[32];
					std::snprintf(age, sizeof(age), "%.0f", group.nav_age);
					out << age << " s";
				}
				if (!group.path)
					out << " no path";
				out << "; blocked by " << GroupBlocker(group, *manager) << ";";
			}
		}
		for (const auto& [key, manager] : info)
			if (!manager.objects_error.empty())
				out << "\n  " << manager.name << ": not read: " << manager.objects_error;
		return out.str();
	}

	// Log section: for each patrol manager below its cap on a size, the spawn points of that size and which
	// of FindSpawnPoint's tests each one fails. Only points still in RemainingSpawnPoints are tried; the
	// list is refilled from all points only once it is empty.
	inline std::string FormatSpawnChecks(const std::map<uint64_t, ManagerInfo>& info)
	{
		static const char* const names[3] = { "small", "medium", "large" };
		std::vector<const ManagerInfo*> list;
		for (const auto& [key, manager] : info)
			if (manager.spawn_read && !manager.spawn_checks.empty())
				list.push_back(&manager);
		std::sort(list.begin(), list.end(), [](const ManagerInfo* a, const ManagerInfo* b) { return a->name < b->name; });

		std::ostringstream out;
		out << "Patrol spawn points of sizes below cap (* = still in the remaining list, the only ones tried):";
		if (list.empty())
			out << " none";
		for (const ManagerInfo* manager : list)
		{
			out << "\n  " << manager->name;
			if (manager->since_try >= 0)
			{
				char since[64];
				std::snprintf(since, sizeof(since), "%.0f s ago (interval %.0f s)", manager->since_try, manager->try_interval);
				out << " (last try " << since << (manager->trying_full ? ", full increase" : "") << ")";
			}
			out << ":";
			for (int size = 0; size < 3; ++size)
			{
				int usable = 0, usable_remaining = 0, shown = 0;
				std::ostringstream points;
				for (const SpawnPointCheck& check : manager->spawn_checks)
				{
					if (check.size != size)
						continue;
					++shown;
					const bool ok = SpawnPointUsable(check);
					usable += ok ? 1 : 0;
					usable_remaining += ok && check.remaining ? 1 : 0;
					char at[96];
					std::snprintf(at, sizeof(at), "%.0f %.0f %.0f", check.x, check.y, check.z);
					points << "\n      " << (check.remaining ? "* " : "  ") << at << ": ";
					if (ok)
						points << "usable";
					else
					{
						std::vector<std::string> fails;
						if (!check.players) fails.push_back("players");
						if (!check.tames) fails.push_back("tames");
						if (!check.structures) fails.push_back("structures");
						if (check.dinos > 0)
						{
							char radius[32];
							std::snprintf(radius, sizeof(radius), "%.0f", ObstructionRadius(size));
							fails.push_back(std::to_string(check.dinos) + " dinos within " + radius +
								(check.nearest.empty() ? std::string() : " (" + check.nearest + ")"));
						}
						points << "fails ";
						for (size_t i = 0; i < fails.size(); ++i)
							points << (i == 0 ? "" : ", ") << fails[i];
					}
				}
				if (shown == 0)
					continue;
				out << "\n    " << names[size] << " " << manager->groups[size] << "/" << manager->max_groups[size] << ": "
					<< manager->points[size] << " points, " << manager->remaining[size] << " remaining, " << usable
					<< " usable (" << usable_remaining << " of them remaining)" << points.str();
			}
		}
		for (const auto& [key, manager] : info)
			if (!manager.spawn_error.empty())
				out << "\n  " << manager.name << ": spawn points not read: " << manager.spawn_error;
		return out.str();
	}

	// Where an id that occupies a patrol connection lives: a group object, a saved group, both, or nowhere.
	inline std::string OccupierKind(int id, const ManagerInfo& manager)
	{
		const bool object = std::find(manager.group_keys.begin(), manager.group_keys.end(), id) != manager.group_keys.end();
		const bool saved = std::any_of(manager.saved.begin(), manager.saved.end(), [id](const SavedGroup& g) { return g.id == id; });
		if (object && saved)
			return "group object, saved";
		if (object)
			return "group object";
		if (saved)
			return manager.saved_read ? "saved only" : "?";
		return manager.saved_read ? "unknown" : "not a group object";
	}

	// Log section: each patrol manager's patrol points, how many connections a new group of each size could
	// start on, and which group ids hold the rest.
	inline std::string FormatPatrolPoints(const std::map<uint64_t, ManagerInfo>& info, size_t max_ids = 12)
	{
		std::vector<const ManagerInfo*> list;
		for (const auto& [key, manager] : info)
			if (manager.patrol_read || !manager.patrol_error.empty())
				list.push_back(&manager);
		std::sort(list.begin(), list.end(), [](const ManagerInfo* a, const ManagerInfo* b) { return a->name < b->name; });

		std::ostringstream out;
		out << "Patrol points (connections free for S/M/L on how many points; occupied by group id x connections):";
		if (list.empty())
			out << " none";
		for (const ManagerInfo* manager : list)
		{
			out << "\n  " << manager->name << ": ";
			if (!manager->patrol_read)
			{
				out << "not read: " << manager->patrol_error;
				continue;
			}
			out << manager->patrol_points << " points" << (manager->patrol_initialized ? "" : " (not initialized)") << ", "
				<< manager->connections << " connections, free S/M/L " << manager->free_connections[0] << "/"
				<< manager->free_connections[1] << "/" << manager->free_connections[2] << " on " << manager->points_with_free[0]
				<< "/" << manager->points_with_free[1] << "/" << manager->points_with_free[2] << " points";
			if (!manager->occupied.empty())
			{
				int total = 0;
				for (const auto& [id, count] : manager->occupied)
					total += count;
				out << "; " << total << " occupied by " << manager->occupied.size() << " ids:";
				for (size_t i = 0; i < manager->occupied.size() && i < max_ids; ++i)
					out << (i == 0 ? " " : ", ") << "#" << manager->occupied[i].first << " x" << manager->occupied[i].second
						<< " (" << OccupierKind(manager->occupied[i].first, *manager) << ")";
				if (manager->occupied.size() > max_ids)
					out << ", +" << (manager->occupied.size() - max_ids) << " more";
			}
		}
		return out.str();
	}

	// [spawn entries S/M/L A/B/C (class default)] when every one matches the class default, otherwise
	// [spawn entries S none (default A), M B, L C]. Empty when not read and no error.
	inline std::string FormatContainers(const ManagerInfo& info)
	{
		if (!info.containers_read)
			return info.containers_error.empty() ? std::string() : "[spawn entries not read: " + info.containers_error + "]";
		bool same = true;
		for (int i = 0; i < 3; ++i)
			same = same && info.containers[i] == info.default_containers[i];
		std::ostringstream out;
		out << "[spawn entries ";
		if (same)
		{
			out << "S/M/L " << info.containers[0] << "/" << info.containers[1] << "/" << info.containers[2] << " (class default)]";
			return out.str();
		}
		for (int i = 0; i < 3; ++i)
		{
			out << (i == 0 ? "" : ", ") << GroupSizeName(i) << " " << info.containers[i];
			if (info.containers[i] != info.default_containers[i])
				out << " (default " << info.default_containers[i] << ")";
		}
		out << "]";
		return out.str();
	}

	inline std::string FormatManagerInfo(const ManagerInfo& info)
	{
		std::ostringstream out;
		out << (info.name.empty() ? std::string("manager") : info.name);
		if (info.has_count)
		{
			char count[32];
			std::snprintf(count, sizeof(count), "%.0f", info.linked_count);
			out << " [linked count " << count << ", links " << info.linked_list << ", reset version "
				<< info.save_version << "/" << info.load_version << "]";
		}
		if (info.has_zone)
		{
			char at[96];
			std::snprintf(at, sizeof(at), "%.0f %.0f %.0f", info.x, info.y, info.z);
			out << " [at " << at << ", " << (info.enabled ? "enabled" : "DISABLED") << ", spawned " << info.spawned
				<< ", desired " << info.min_desired << ", max " << info.max_npc << ", entries " << info.entries << "]";
		}
		if (info.has_groups)
			out << " " << FormatGroupSummary(info);
		else if (!info.groups_error.empty())
			out << " [groups not read: " << info.groups_error << "]";
		if (const std::string containers = FormatContainers(info); !containers.empty())
			out << " " << containers;
		return out.str();
	}

	struct ManagerStats
	{
		uint64_t manager = 0;
		size_t riders = 0;
		size_t riders_mounted = 0;
		size_t mounts = 0;
		size_t mounts_ridden = 0;
		size_t still_unmounted = 0;   // unmounted riders that did not move since the last scan
		size_t Unmounted() const { return riders - riders_mounted; }
	};

	struct Report
	{
		std::vector<ManagerStats> managers;  // most unmounted riders first
		ManagerStats unlinked;               // patrol creatures with no manager link
		ManagerStats mission;                // garrisons and other mission creatures
	};

	inline void Add(ManagerStats& stats, const Sighting& sighting, double still_distance)
	{
		if (sighting.role == Role::Rider)
		{
			++stats.riders;
			if (sighting.mounted)
				++stats.riders_mounted;
			else if (sighting.known && sighting.moved < still_distance)
				++stats.still_unmounted;
		}
		else
		{
			++stats.mounts;
			if (sighting.mounted)
				++stats.mounts_ridden;
		}
	}

	inline Report BuildReport(const std::vector<Sighting>& sightings, double still_distance)
	{
		Report report;
		std::map<uint64_t, ManagerStats> by_manager;
		for (const Sighting& sighting : sightings)
		{
			if (sighting.mission)
				Add(report.mission, sighting, still_distance);
			else if (sighting.manager == 0)
				Add(report.unlinked, sighting, still_distance);
			else
			{
				ManagerStats& stats = by_manager[sighting.manager];
				stats.manager = sighting.manager;
				Add(stats, sighting, still_distance);
			}
		}
		for (auto& [key, stats] : by_manager)
			report.managers.push_back(stats);
		std::stable_sort(report.managers.begin(), report.managers.end(), [](const ManagerStats& a, const ManagerStats& b)
			{
				return a.Unmounted() > b.Unmounted();
			});
		return report;
	}

	inline std::string FormatStats(const ManagerStats& stats)
	{
		std::ostringstream out;
		out << stats.riders << " riders (" << stats.riders_mounted << " mounted, " << stats.Unmounted() << " unmounted";
		if (stats.still_unmounted > 0)
			out << ", " << stats.still_unmounted << " unmounted and not moving";
		out << "), " << stats.mounts << " mounts (" << stats.mounts_ridden << " ridden)";
		return out.str();
	}

	inline std::string FormatReport(const Report& report, const std::map<uint64_t, ManagerInfo>& info, int max_managers)
	{
		std::ostringstream out;
		size_t riders = report.unlinked.riders, unmounted = report.unlinked.Unmounted();
		for (const ManagerStats& stats : report.managers)
		{
			riders += stats.riders;
			unmounted += stats.Unmounted();
		}
		out << "Patrol creatures without a mission: " << riders << " riders, " << unmounted << " unmounted, across "
			<< report.managers.size() << " manager(s)\n";
		out << "Mission-owned (garrisons): " << FormatStats(report.mission) << "\n";
		if (report.unlinked.riders + report.unlinked.mounts > 0)
			out << "No manager link: " << FormatStats(report.unlinked) << "\n";

		int shown = 0;
		for (const ManagerStats& stats : report.managers)
		{
			if (shown++ >= max_managers)
				break;
			const auto found = info.find(stats.manager);
			out << "  " << (found != info.end() ? FormatManagerInfo(found->second) : std::string("manager"));
			out << ": " << FormatStats(stats) << "\n";
		}
		if (static_cast<int>(report.managers.size()) > max_managers)
			out << "  ... " << report.managers.size() - max_managers << " more in the log\n";

		// Patrol managers found in the level with no patrol creature linked to them: an empty roster.
		std::vector<const ManagerInfo*> empty;
		for (const auto& [key, manager] : info)
		{
			if (!manager.has_zone)
				continue;
			const bool has_creatures = std::any_of(report.managers.begin(), report.managers.end(),
				[key = key](const ManagerStats& stats) { return stats.manager == key; });
			if (!has_creatures)
				empty.push_back(&manager);
		}
		std::sort(empty.begin(), empty.end(), [](const ManagerInfo* a, const ManagerInfo* b) { return a->name < b->name; });
		out << "Patrol managers with no patrol creatures: " << empty.size() << "\n";
		int listed = 0;
		for (const ManagerInfo* manager : empty)
		{
			if (listed++ >= max_managers)
			{
				out << "  ... " << empty.size() - max_managers << " more in the log\n";
				break;
			}
			out << "  " << FormatManagerInfo(*manager) << "\n";
		}
		return out.str();
	}

	// ------------------------------------------------------------------------------------------
	// Clean plan
	// ------------------------------------------------------------------------------------------

	inline bool EndsWithNoCase(std::string_view text, std::string_view ending)
	{
		return !ending.empty() && text.size() >= ending.size() && ToLower(text.substr(text.size() - ending.size())) == ToLower(ending);
	}

	struct CleanEntry
	{
		uint64_t creature = 0;
		Role role = Role::Rider;
	};

	struct CleanPlan
	{
		std::vector<CleanEntry> remove;
		size_t unlinked_riders = 0;     // riders without a manager link
		size_t unlinked_mounts = 0;     // unridden mounts without a manager link
		size_t riders_on_foot = 0;      // a manager's riders not on a mount
		size_t empty_mounts = 0;        // a manager's mounts nobody rides
	};

	// A save load takes every patrol rider off its mount and only some get back on; the rest stand next
	// to empty mounts, fill the manager's roster and keep it from sending out new patrols. Each load also
	// cuts some riders off from their manager for good. A clean removes what is not a mounted pair, so
	// the managers send out fresh patrols without touching the pairs that work, as long as their
	// linked count isn't negative (see Clean.ResetNegativeLinkedCount). Mission creatures and mounted
	// pairs are never removed.
	inline CleanPlan PlanClean(const std::vector<Sighting>& sightings, bool remove_unlinked, bool remove_unpaired)
	{
		CleanPlan plan;
		for (const Sighting& sighting : sightings)
		{
			if (sighting.mission || sighting.mounted)
				continue;
			const bool rider = sighting.role == Role::Rider;
			if (sighting.manager == 0)
			{
				if (!remove_unlinked)
					continue;
				++(rider ? plan.unlinked_riders : plan.unlinked_mounts);
			}
			else
			{
				if (!remove_unpaired)
					continue;
				++(rider ? plan.riders_on_foot : plan.empty_mounts);
			}
			plan.remove.push_back({ sighting.creature, sighting.role });
		}
		return plan;
	}
}
