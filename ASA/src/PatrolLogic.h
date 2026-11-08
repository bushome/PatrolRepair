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
	};

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
