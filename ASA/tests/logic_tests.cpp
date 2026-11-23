// Stand-alone tests for src/PatrolLogic.h. No game, AsaApi DLL or Windows needed.
//
// Build and run from the project folder (any C++20 compiler):
//   cl /std:c++20 /EHsc /I extern\AsaApi\AsaApi\Core\Public /I src tests\logic_tests.cpp && logic_tests.exe configs\config.json
//   clang++ -std=c++20 -I extern/AsaApi/AsaApi/Core/Public -I src tests/logic_tests.cpp -o logic_tests && ./logic_tests configs/config.json
//
// The optional argument is a config.json to validate. Add --shipped after it to also check the file
// against the values of the shipped config.

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>

#include "PatrolLogic.h"

using namespace PatrolRepair;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(condition) \
	do { \
		++g_checks; \
		if (!(condition)) { \
			++g_failures; \
			std::cerr << "FAILED line " << __LINE__ << ": " #condition "\n"; \
		} \
	} while (false)

static Settings Parse(const char* text)
{
	return ParseSettings(nlohmann::json::parse(text, nullptr, true, true));
}

static void TestParsing()
{
	CHECK(Parse("{}").logging);
	CHECK(!Parse(R"({ "General": { "Logging": false } })").logging);
	CHECK(Parse(R"({ "General": { "Logging": "no" } })").logging); // not a boolean: default kept
	bool threw = false;
	try { Parse("[]"); } catch (const std::exception&) { threw = true; }
	CHECK(threw);

	const Settings defaults = Parse("{}");
	CHECK(defaults.warnings.empty());
	CHECK(defaults.scan_actors_per_tick == 5000);
	CHECK(defaults.rider_family == "Thrall_Character_BP_Base_C");

	const Settings bad = Parse(R"({ "General": { "ScanActorsPerTick": 5, "ReportManagers": "x" }, "Patrols": { "RiderFamily": "" } })");
	CHECK(bad.scan_actors_per_tick == 5000);
	CHECK(bad.report_managers == 10);
	CHECK(bad.rider_family == "Thrall_Character_BP_Base_C");
	CHECK(bad.warnings.size() == 3);

	CHECK(defaults.removals_per_tick == 10 && defaults.remove_unlinked && defaults.remove_unpaired && defaults.clean_after_boot_minutes == 10 && defaults.auto_clean_minutes == 0);
	CHECK(defaults.mount_suffix == "_Thrall_C");
	const Settings clean = Parse(R"({ "Clean": { "RemovalsPerTick": 25, "RemoveUnlinked": false, "AutoCleanMinutes": 30 } })");
	CHECK(clean.removals_per_tick == 25 && !clean.remove_unlinked && clean.auto_clean_minutes == 30);
	CHECK(clean.warnings.empty());
	const Settings bad_clean = Parse(R"({ "Clean": { "RemovalsPerTick": 0, "RemoveUnlinked": "yes", "AutoCleanMinutes": -1 } })");
	CHECK(bad_clean.removals_per_tick == 10 && bad_clean.remove_unlinked && bad_clean.auto_clean_minutes == 0);
	CHECK(bad_clean.warnings.size() == 3);
	CHECK(defaults.reset_negative_count);
	const Settings no_reset = Parse(R"({ "Clean": { "ResetNegativeLinkedCount": false } })");
	CHECK(!no_reset.reset_negative_count && no_reset.warnings.empty());
	CHECK(defaults.restore_spawn_entries);
	const Settings no_restore = Parse(R"({ "Spawning": { "RestoreSpawnEntries": false } })");
	CHECK(!no_restore.restore_spawn_entries && no_restore.warnings.empty());
	const Settings bad_restore = Parse(R"({ "Spawning": { "RestoreSpawnEntries": 1 } })");
	CHECK(bad_restore.restore_spawn_entries && bad_restore.warnings.size() == 1);
	CHECK(defaults.dissolve_orphan_groups && defaults.scan_after_boot_minutes == 3);
	const Settings no_dissolve = Parse(R"({ "Spawning": { "DissolveOrphanGroups": false, "ScanAfterBootMinutes": 0 } })");
	CHECK(!no_dissolve.dissolve_orphan_groups && no_dissolve.scan_after_boot_minutes == 0 && no_dissolve.warnings.empty());
	const Settings bad_boot = Parse(R"({ "Spawning": { "ScanAfterBootMinutes": -1 } })");
	CHECK(bad_boot.scan_after_boot_minutes == 3 && bad_boot.warnings.size() == 1);
}

static Sighting At(uint64_t creature, uint64_t manager, Role role, bool mounted, double x, bool mission = false)
{
	Sighting s;
	s.creature = creature;
	s.manager = manager;
	s.role = role;
	s.mounted = mounted;
	s.x = x;
	s.mission = mission;
	return s;
}

static bool Removes(const CleanPlan& plan, uint64_t creature)
{
	return std::find_if(plan.remove.begin(), plan.remove.end(), [&](const CleanEntry& e) { return e.creature == creature; }) != plan.remove.end();
}

static void TestClean()
{
	const std::vector<Sighting> sightings = {
		// Manager 1: a ridden pair, two empty mounts, riders on foot.
		At(1, 1, Role::Rider, true, 0), At(2, 1, Role::Mount, true, 0),
		At(3, 1, Role::Mount, false, 1000), At(4, 1, Role::Mount, false, 5000),
		At(10, 1, Role::Rider, false, 1010), At(11, 1, Role::Rider, false, 20000),
		// No manager link: a rider on foot, an empty mount and a ridden pair.
		At(30, 0, Role::Rider, false, 0), At(31, 0, Role::Mount, false, 0),
		At(32, 0, Role::Rider, true, 0), At(33, 0, Role::Mount, true, 0),
		// Garrison: never touched.
		At(40, 3, Role::Rider, false, 0, true), At(41, 0, Role::Mount, false, 0, true),
	};

	const CleanPlan plan = PlanClean(sightings, true, true);
	CHECK(Removes(plan, 3) && Removes(plan, 4));                 // empty mounts of a manager
	CHECK(Removes(plan, 10) && Removes(plan, 11));               // riders on foot of a manager
	CHECK(Removes(plan, 30) && Removes(plan, 31));               // unlinked rider and empty mount
	CHECK(!Removes(plan, 1) && !Removes(plan, 2) && !Removes(plan, 32) && !Removes(plan, 33));  // mounted pairs stay
	CHECK(!Removes(plan, 40) && !Removes(plan, 41));             // mission creatures stay
	CHECK(plan.empty_mounts == 2 && plan.riders_on_foot == 2 && plan.unlinked_riders == 1 && plan.unlinked_mounts == 1);
	CHECK(plan.remove.size() == 6);

	const CleanPlan only_managers = PlanClean(sightings, false, true);
	CHECK(only_managers.remove.size() == 4 && !Removes(only_managers, 30));
	const CleanPlan only_unlinked = PlanClean(sightings, true, false);
	CHECK(only_unlinked.remove.size() == 2 && !Removes(only_unlinked, 3) && !Removes(only_unlinked, 10));
	CHECK(PlanClean(sightings, false, false).remove.empty());

	CHECK(EndsWithNoCase("Raptor_Character_BP_Thrall_C", "_thrall_c"));
	CHECK(!EndsWithNoCase("Raptor_Character_BP_C", "_Thrall_C"));
}

static void TestOrphanGroups()
{
	ManagerInfo info;
	info.has_groups = true;
	info.saved_read = true;
	info.validated = true;
	info.live_groups = 1;
	info.group_keys = { 20 };
	const auto saved = [](int id, int latent, int live_any) {
		SavedGroup group;
		group.id = id;
		group.latent = latent;
		group.live_any = live_any;
		return group;
	};
	// 10 orphan; 20 has a group object; 30 latent; 40 has a live creature; 0 invalid id; 50 orphan
	info.saved = { saved(10, 0, 0), saved(20, 0, 0), saved(30, 2, 0), saved(40, 0, 1), saved(0, 0, 0), saved(50, 0, 0), saved(10, 0, 0) };
	const std::vector<int> ids = OrphanGroups(info);
	CHECK(ids == std::vector<int>({ 10, 50 }));
	ManagerInfo not_validated = info;
	not_validated.validated = false;
	CHECK(OrphanGroups(not_validated).empty());
	ManagerInfo keys_unread = info;
	keys_unread.live_groups = -1;
	CHECK(OrphanGroups(keys_unread).empty());
	ManagerInfo keys_short = info;
	keys_short.live_groups = 2;  // a key not recorded: don't guess
	CHECK(OrphanGroups(keys_short).empty());
	ManagerInfo saved_unread = info;
	saved_unread.saved_read = false;
	CHECK(OrphanGroups(saved_unread).empty());
}

static void TestReport()
{
	auto rider = [](uint64_t manager, bool mounted, bool known = false, double moved = 0, bool mission = false)
		{
			Sighting s;
			s.manager = manager;
			s.role = Role::Rider;
			s.mounted = mounted;
			s.known = known;
			s.moved = moved;
			s.mission = mission;
			return s;
		};
	auto mount = [](uint64_t manager, bool ridden, bool mission = false)
		{
			Sighting s;
			s.manager = manager;
			s.role = Role::Mount;
			s.mounted = ridden;
			s.mission = mission;
			return s;
		};

	std::vector<Sighting> sightings = {
		rider(1, true), rider(1, false, true, 10), rider(1, false, true, 5000), mount(1, true),
		rider(2, false, true, 0), rider(2, false), rider(2, false), rider(2, false),
		rider(0, false),
		rider(3, false, false, 0, true), mount(3, true, true),
	};
	const Report report = BuildReport(sightings, 300);
	CHECK(report.managers.size() == 2);
	CHECK(report.managers[0].manager == 2);               // most unmounted first
	CHECK(report.managers[0].Unmounted() == 4);
	CHECK(report.managers[0].still_unmounted == 1);
	CHECK(report.managers[1].riders == 3 && report.managers[1].riders_mounted == 1);
	CHECK(report.managers[1].still_unmounted == 1);       // the one that moved 5000 does not count
	CHECK(report.managers[1].mounts == 1 && report.managers[1].mounts_ridden == 1);
	CHECK(report.unlinked.riders == 1);
	CHECK(report.mission.riders == 1 && report.mission.mounts == 1);

	std::map<uint64_t, ManagerInfo> info;
	info[2] = { "NPCZoneManager_City", true, 40, 37, 3, 3 };
	const std::string text = FormatReport(report, info, 1);
	CHECK(text.find("8 riders, 7 unmounted, across 2 manager(s)") != std::string::npos);
	CHECK(text.find("NPCZoneManager_City [linked count 40, links 37, reset version 3/3]: 4 riders") != std::string::npos);
	CHECK(text.find("1 more in the log") != std::string::npos);
	CHECK(text.find("No manager link: 1 riders") != std::string::npos);
	CHECK(text.find("Patrol managers with no patrol creatures: 0") != std::string::npos);

	// Managers found in the level with no creature linked are listed on their own, with location and state.
	ManagerInfo empty{ "NPCZoneManager_Patrols_C_Empty", true, -12, 0, 1, 1 };
	empty.has_zone = true;
	empty.x = 100; empty.y = -200; empty.z = 30;
	empty.enabled = true; empty.spawned = 0; empty.min_desired = 8; empty.max_npc = 20; empty.entries = 3;
	info[9] = empty;
	info[2].has_zone = true;
	const std::string full = FormatReport(report, info, 1 << 30);
	CHECK(full.find("Patrol managers with no patrol creatures: 1") != std::string::npos);
	CHECK(full.find("NPCZoneManager_Patrols_C_Empty [linked count -12, links 0, reset version 1/1] [at 100 -200 30, enabled, spawned 0, desired 8, max 20, entries 3]") != std::string::npos);
	CHECK(full.find("NPCZoneManager_City [linked count 40, links 37, reset version 3/3] [at 0 0 0, DISABLED, spawned 0") != std::string::npos);
	CHECK(full.find("[groups") == std::string::npos);  // not shown when not read

	// Patrol group bookkeeping of the Blueprint: counts against caps, group objects, saved groups.
	ManagerInfo groups{ "NPCZoneManager_Patrols_C_Stuck", true, 0, 0, 1, 1 };
	groups.has_groups = true;
	groups.groups[0] = 8; groups.groups[1] = 4;
	groups.max_groups[0] = 8; groups.max_groups[1] = 4;
	groups.live_groups = 0;
	groups.validated = true;
	groups.saved_read = true;
	groups.saved = { { 101, 0, 3, 0, 0, 0 }, { 102, 1, 0, 2, 1, 4 }, { 103, 7, 1, 0, 0, 0 } };
	CHECK(FormatManagerInfo(groups).find("reset version 1/1] [groups small 8/8, medium 4/4, large 0/0 (at cap), group objects 0, "
		"saved S/M/L 1/1/0 (2 with no live member), validated, delay 0 s]") != std::string::npos);
	std::map<uint64_t, ManagerInfo> group_info;
	group_info[5] = groups;
	group_info[6] = { "NPCZoneManager_Patrols_C_Empty", true, 0, 0, 1, 1 };
	const std::string saved = FormatSavedGroups(group_info);
	CHECK(saved.find("\n  NPCZoneManager_Patrols_C_Stuck: #101 S 3/0 live 0/0; #102 M 0/2 live 1/4; #103 size 7 1/0 live 0/0;") != std::string::npos);
	CHECK(saved.find("_Empty") == std::string::npos);
	CHECK(FormatSavedGroups({}) == "Saved patrol groups (#id size members/latent, live linked/anywhere): none");

	ManagerInfo below{ "Below", true, 0, 0, 1, 1 };
	below.has_groups = true;
	below.max_groups[0] = 8;
	below.groups[0] = 3;
	below.manual = false;
	below.initial_delay = 12.4;
	below.groups_error = "no SavedGroups";
	const std::string below_text = FormatManagerInfo(below);
	CHECK(below_text.find("[groups small 3/8, medium 0/0, large 0/0, group objects ?, saved groups not read: no SavedGroups, "
		"not validated, not manual, delay 12 s]") != std::string::npos);

	// A delay just below 0 prints as 0.
	below.initial_delay = -0.3;
	CHECK(FormatManagerInfo(below).find("delay 0 s]") != std::string::npos);

	// Group objects: what keeps ValidateGroup from dissolving each one.
	ManagerInfo objects{ "NPCZoneManager_Patrols_C_Objects", true, 0, 0, 1, 1 };
	objects.saved_read = true;
	objects.saved = { { 11, 0, 0, 0, 0, 0 }, { 12, 1, 0, 2, 0, 0 } };
	objects.objects_read = true;
	GroupObject empty_group;  // nothing blocks it
	empty_group.key = 11; empty_group.id = 11; empty_group.nav_read = true; empty_group.nav_never = true; empty_group.path = true;
	empty_group.saved_index = 0;
	GroupObject waiting = empty_group;  // its own NumMembers still 3, saved entry latent 2
	waiting.key = 12; waiting.id = 12; waiting.size = 1; waiting.num_members = 3; waiting.saved_index = 1;
	waiting.nav_never = false; waiting.nav_age = 42.4; waiting.path = false;
	GroupObject orphan = empty_group;   // not in SavedGroups, latent on the object
	orphan.key = 13; orphan.id = 14; orphan.size = 2; orphan.latent = 1; orphan.saved_index = -1;
	orphan.listed = 2; orphan.valid = 1; orphan.awake = 1; orphan.nav_read = false;
	objects.objects = { empty_group, waiting, orphan };
	CHECK(GroupBlocker(empty_group, objects) == "nothing (should dissolve)");
	CHECK(GroupBlocker(waiting, objects) == "awake 0 < NumMembers 3, nav updated 42 s ago, saved latent 2");
	CHECK(GroupBlocker(orphan, objects) == "latent 1, not in SavedGroups, so dissolving would not count it down");
	GroupObject old_nav = waiting;
	old_nav.nav_age = 181; old_nav.num_members = 0; old_nav.saved_index = 0;
	CHECK(GroupBlocker(old_nav, objects) == "nothing (should dissolve)");
	GroupObject awake = old_nav;
	awake.awake = 2; awake.valid = 2; awake.listed = 2;
	CHECK(GroupBlocker(awake, objects) == "nothing unless a member has a target");
	std::map<uint64_t, ManagerInfo> object_info;
	object_info[1] = objects;
	ManagerInfo broken{ "Broken", true, 0, 0, 1, 1 };
	broken.objects_error = "no NumMembers on PatrolGroupManager_C";
	object_info[2] = broken;
	const std::string object_text = FormatGroupObjects(object_info);
	CHECK(object_text.find("\n  NPCZoneManager_Patrols_C_Objects: #11 S 0/0 members 0/0/0 nav never; blocked by nothing (should dissolve);"
		" #12 M 3/0 members 0/0/0 nav 42 s no path; blocked by awake 0 < NumMembers 3, nav updated 42 s ago, saved latent 2;"
		" #14 (key 13) L 0/1 members 2/1/1 nav ?; blocked by latent 1, not in SavedGroups") != std::string::npos);
	CHECK(object_text.find("\n  Broken: not read: no NumMembers on PatrolGroupManager_C") != std::string::npos);
	CHECK(FormatGroupObjects({}) == "Patrol group objects (#id size NumMembers/latent, members listed/valid/awake, nav; blocked by): none");

	// Spawn points of sizes below cap.
	ManagerInfo spawn{ "NPCZoneManager_Patrols_C_Spawn", true, 0, 0, 1, 1 };
	spawn.has_groups = true;
	spawn.groups[0] = 5; spawn.max_groups[0] = 6;
	spawn.groups[1] = 3; spawn.max_groups[1] = 3;
	spawn.spawn_read = true;
	spawn.points[0] = 3; spawn.points[1] = 2; spawn.points[2] = 1;
	spawn.remaining[0] = 2;
	spawn.since_try = 4.2; spawn.try_interval = 18;
	SpawnPointCheck blocked;
	blocked.x = 1; blocked.y = -2; blocked.z = 3; blocked.remaining = true; blocked.dinos = 2; blocked.nearest = "Thrall_Fighter_C";
	SpawnPointCheck near_player;
	near_player.x = 10; near_player.remaining = true; near_player.players = false; near_player.structures = false;
	SpawnPointCheck free_point;
	free_point.x = 20;
	spawn.spawn_checks = { blocked, near_player, free_point };
	CHECK(!SpawnPointUsable(blocked) && !SpawnPointUsable(near_player) && SpawnPointUsable(free_point));
	CHECK(ObstructionRadius(0) == 500 && ObstructionRadius(1) == 800 && ObstructionRadius(2) == 1200);
	CHECK(FormatManagerInfo(spawn).find("group objects -1") == std::string::npos);
	CHECK(FormatManagerInfo(spawn).find(", spawn points S/M/L 3/2/1, not validated") != std::string::npos);
	std::map<uint64_t, ManagerInfo> spawn_info;
	spawn_info[1] = spawn;
	const std::string spawn_text = FormatSpawnChecks(spawn_info);
	CHECK(spawn_text.find("\n  NPCZoneManager_Patrols_C_Spawn (last try 4 s ago (interval 18 s)):"
		"\n    small 5/6: 3 points, 2 remaining, 1 usable (0 of them remaining)"
		"\n      * 1 -2 3: fails 2 dinos within 500 (Thrall_Fighter_C)"
		"\n      * 10 0 0: fails players, structures"
		"\n        20 0 0: usable") != std::string::npos);
	CHECK(spawn_text.find("medium") == std::string::npos);
	CHECK(FormatSpawnChecks({}) == "Patrol spawn points of sizes below cap (* = still in the remaining list, the only ones tried): none");

	// Patrol points and who occupies their connections.
	ManagerInfo points{ "NPCZoneManager_Patrols_C_Points", true, 0, 0, 1, 1 };
	points.patrol_read = true;
	points.patrol_points = 40;
	points.patrol_initialized = true;
	points.connections = 90;
	points.free_connections[0] = 2;
	points.points_with_free[0] = 1;
	points.saved_read = true;
	points.saved = { { 7, 0, 0, 0, 0, 0 }, { 8, 1, 0, 0, 0, 0 } };
	points.group_keys = { 7, 9 };
	points.occupied = { { 7, 30 }, { 8, 20 }, { 9, 10 }, { 5, 3 } };
	CHECK(OccupierKind(7, points) == "group object, saved");
	CHECK(OccupierKind(8, points) == "saved only");
	CHECK(OccupierKind(9, points) == "group object");
	CHECK(OccupierKind(5, points) == "unknown");
	std::map<uint64_t, ManagerInfo> point_info;
	point_info[1] = points;
	ManagerInfo no_points{ "NoPoints", true, 0, 0, 1, 1 };
	no_points.patrol_error = "no Connections on PrimalPatrolPoint";
	point_info[2] = no_points;
	const std::string point_text = FormatPatrolPoints(point_info, 3);
	CHECK(point_text.find("\n  NPCZoneManager_Patrols_C_Points: 40 points, 90 connections, free S/M/L 2/0/0 on 1/0/0 points; "
		"63 occupied by 4 ids: #7 x30 (group object, saved), #8 x20 (saved only), #9 x10 (group object), +1 more") != std::string::npos);
	CHECK(point_text.find("\n  NoPoints: not read: no Connections on PrimalPatrolPoint") != std::string::npos);
	CHECK(FormatPatrolPoints({}).find(": none") != std::string::npos);

	ManagerInfo unread{ "Unread", true, 0, 0, 1, 1 };
	unread.groups_error = "no NumSmallGroups";
	CHECK(FormatManagerInfo(unread).find("[groups not read: no NumSmallGroups]") != std::string::npos);
	CHECK(FormatManagerInfo(unread).find("spawn entries") == std::string::npos);

	ManagerInfo entries{ "Entries" };
	entries.containers_read = true;
	entries.containers[0] = entries.default_containers[0] = "SmallA";
	entries.containers[1] = entries.default_containers[1] = "MediumA";
	entries.containers[2] = entries.default_containers[2] = "LargeA";
	CHECK(FormatContainers(entries) == "[spawn entries S/M/L SmallA/MediumA/LargeA (class default)]");
	entries.containers[0] = "none";
	entries.containers[2] = "none";
	CHECK(FormatContainers(entries) == "[spawn entries S none (default SmallA), M MediumA, L none (default LargeA)]");
	CHECK(FormatManagerInfo(entries).find("[spawn entries S none (default SmallA)") != std::string::npos);
	ManagerInfo entries_error{ "EntriesError" };
	entries_error.containers_error = "no NPCSpawnEntriesGroupSmall";
	CHECK(FormatContainers(entries_error) == "[spawn entries not read: no NPCSpawnEntriesGroupSmall]");
}

static void TestConfigFile(const std::string& path, bool shipped)
{
	std::ifstream file(path);
	CHECK(file.is_open());
	if (!file.is_open())
		return;
	Settings settings;
	try
	{
		settings = ParseSettings(nlohmann::json::parse(file, nullptr, true, true));
	}
	catch (const std::exception& error)
	{
		++g_checks;
		++g_failures;
		std::cerr << "FAILED: " << path << " did not parse: " << error.what() << "\n";
		return;
	}
	for (const std::string& warning : settings.warnings)
		std::cerr << "warning: " << warning << "\n";
	CHECK(settings.warnings.empty());
	if (!shipped)
		return;
	CHECK(settings.scan_actors_per_tick == 5000);
	CHECK(settings.still_distance == 300.0);
	CHECK(settings.report_managers == 10);
	CHECK(settings.rider_family == "Thrall_Character_BP_Base_C");
	CHECK(settings.mount_path == "/Thrall/Characters/RideableCreatures/");
	CHECK(settings.removals_per_tick == 10);
	CHECK(settings.remove_unlinked);
	CHECK(settings.remove_unpaired);
	CHECK(settings.clean_after_boot_minutes == 10);
	CHECK(settings.mount_suffix == "_Thrall_C");
	CHECK(settings.auto_clean_minutes == 0);
	CHECK(settings.restore_spawn_entries);
	CHECK(settings.dissolve_orphan_groups);
	CHECK(settings.scan_after_boot_minutes == 3);
}

int main(int argc, char** argv)
{
	TestParsing();
	TestOrphanGroups();
	TestReport();
	TestClean();
	if (argc > 1)
		TestConfigFile(argv[1], argc > 2 && std::string(argv[2]) == "--shipped");
	std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
