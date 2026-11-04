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
}

int main(int argc, char** argv)
{
	TestParsing();
	TestReport();
	TestClean();
	if (argc > 1)
		TestConfigFile(argv[1], argc > 2 && std::string(argv[2]) == "--shipped");
	std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
