// PatrolRepair - reports Lost Colony patrols per patrol manager: how many riders and mounts each one has,
// how many riders are left without a mount, and how many of those have stopped moving.
//
// How it works:
//   - PatrolRepair.Report prints the last finished scan and starts a new one. A scan walks the level
//     actor lists a slice per second (ScanActorsPerTick), so it never lands in one server tick.
//   - Riders are creatures deriving from Patrols.RiderFamily; mounts are creatures whose class path
//     contains Patrols.MountPathContains or whose class name ends with Patrols.MountClassEndsWith. Each
//     is grouped by the zone manager it is linked to (APrimalDinoCharacter::DirectLinkNPCZoneManager);
//     garrison creatures of outposts (OwnerMission set) are counted separately.
//   - A rider counts as mounted when any of four links says so: its MountedDino, the actor its root
//     component is attached to, a mount's PassengerPerSeat, or a mount's CarriedCharacter. The log line
//     after each report says how many pairs each link found, and which non-patrol classes riders sit on.
//   - Each manager's own bookkeeping is shown next to what the scan found: DirectLinkDinoCount (what it
//     believes it has out), DirectLinkedDinos (links it still holds) and its patrol reset versions.
//   - PatrolRepair.Clean plans from the last scan: everything that is not a mounted pair. A save load
//     takes every patrol rider off its mount and only some get back on; riders on foot and empty mounts
//     then fill the manager's roster so it never sends out new patrols, and each load cuts some riders
//     off from their manager for good. Removing them (Clean.RemoveUnpaired, Clean.RemoveUnlinked) lets
//     the managers send out fresh mounted patrols. With "confirm" the plan is queued and removed
//     Clean.RemovalsPerTick per second, each creature checked again just before. Clean.CleanAfterBootMinutes
//     runs a scan and clean once after load, Clean.AutoCleanMinutes on an interval.
//   - Each patrol manager's group bookkeeping comes from its Blueprint (NPCZoneManager_Patrols_C), read by
//     name through the class's reflection data: group counts against their caps, group objects in
//     memory, and SavedGroups (the only part kept in the save) with the live creatures carrying each id.
//     Each group object's own NumMembers, LatentSpawnCount, members and navigation time show what keeps
//     the manager's ValidateGroup from dissolving it. For each size a manager is below its cap on, every
//     spawn point of that size is put through the tests the Blueprint's FindSpawnPoint makes (the game's
//     own player, tame and structure distance checks, and no dino within the size's radius). Each
//     manager's patrol points show how many of their connections a new group could start on and which
//     group ids hold the rest (a group starts only on a connection no group occupies).
//   - A manager whose linked count (DirectLinkDinoCount) has gone negative sends out no patrols at all.
//     Clean.ResetNegativeLinkedCount sets it back to 0 after each scan. Apart from that, the cleans and
//     ForceReset, nothing else changes the world.
//   - The native ANPCZoneManager::SpawnPatrolGroup is hooked for a trace: the first calls per manager per
//     load, how many creatures each spawned, and each spawn entries container's creature classes. A patrol
//     manager loaded from a save can hand it no container (its NPCSpawnEntriesGroupSmall/Medium/Large are
//     empty), so every group it sends out has no members. With Spawning.RestoreSpawnEntries the empty fields
//     are set from the class default and the call gets the container for its group's size.
//
// Everything runs on the game thread (AsaApi's one-second timer callback and the commands).

#include "API/ARK/Ark.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "PatrolLogic.h"

#ifndef PROJECT_NAME
#define PROJECT_NAME "PatrolRepair"
#endif

namespace
{
	// General.Logging = false silences every line this plugin writes to the API log and the console.
	bool g_logging_enabled = true;

	struct GatedLog
	{
		template <typename... Args> void info(Args&&... args) const { if (g_logging_enabled) Log::GetLog()->info(std::forward<Args>(args)...); }
		template <typename... Args> void warn(Args&&... args) const { if (g_logging_enabled) Log::GetLog()->warn(std::forward<Args>(args)...); }
		template <typename... Args> void error(Args&&... args) const { if (g_logging_enabled) Log::GetLog()->error(std::forward<Args>(args)...); }
	};
	const GatedLog* PluginLog() { static const GatedLog gate; return &gate; }

	using namespace PatrolRepair;
	using Clock = std::chrono::steady_clock;

	const char* const kCmdReload = "PatrolRepair.Reload";
	const char* const kCmdReport = "PatrolRepair.Report";
	const char* const kCmdClean = "PatrolRepair.Clean";
	const char* const kCmdForceReset = "PatrolRepair.ForceReset";
	const char* const kTimerId = "PatrolRepair.Timer";
	const char* const kLevelActorsField = "ULevel.Actors2";
	constexpr double kMaxPlanAgeSeconds = 600;  // a manual clean needs a scan at most this old
	constexpr int kFirstPlayerTeam = 50000;

	struct ClassKind
	{
		bool rider = false;
		bool mount = false;
	};

	// Ways a rider and its mount can be linked; the report says which ones the game actually uses.
	enum Link
	{
		LinkMountedDino,
		LinkAttachParent,
		LinkPassengerSeat,
		LinkCarried,
		LinkCount,
	};

	const char* const kLinkNames[LinkCount] = { "MountedDino", "attach parent", "passenger seat", "carried" };

	struct Pair
	{
		uint64_t rider = 0;
		uint64_t mount = 0;
		Link link = LinkMountedDino;
		std::string mount_class;  // filled for attach parents, to name mounts outside MountPathContains
	};

	Settings g_settings;
	bool g_settings_loaded = false;
	std::unordered_map<UClass*, ClassKind> g_kinds;

	// Scan in progress.
	bool g_scan_running = false;
	size_t g_scan_level = 0;
	int32 g_scan_actor = 0;
	Clock::time_point g_scan_started{};
	std::vector<Sighting> g_sightings;
	std::unordered_map<uint64_t, size_t> g_rider_index;       // creature -> sighting
	std::unordered_map<uint64_t, size_t> g_mount_index;       // creature -> sighting
	std::vector<Pair> g_pairs;
	std::map<uint64_t, ManagerInfo> g_managers;
	std::unordered_map<uint64_t, TWeakObjectPtr<APrimalDinoCharacter>> g_refs;
	std::unordered_map<uint64_t, FVector> g_positions_now;
	std::unordered_map<uint64_t, FVector> g_positions_before; // from the previous finished scan
	std::map<std::pair<uint64_t, int>, int> g_group_linked;   // (manager, PatrolGroupID) -> creatures
	std::unordered_map<int, int> g_group_any;                 // PatrolGroupID -> creatures, any link

	// Last finished scan.
	bool g_have_report = false;
	std::vector<Sighting> g_last_sightings;
	std::unordered_map<uint64_t, TWeakObjectPtr<APrimalDinoCharacter>> g_last_refs;
	// Patrol managers seen by the current and the last finished scan (for ForceReset).
	std::unordered_map<uint64_t, TWeakObjectPtr<ANPCZoneManager>> g_manager_refs;
	std::unordered_map<uint64_t, TWeakObjectPtr<ANPCZoneManager>> g_last_manager_refs;
	std::map<uint64_t, ManagerInfo> g_last_managers;

	// Clean in progress.
	struct QueuedCreature
	{
		TWeakObjectPtr<APrimalDinoCharacter> creature;
		uint64_t manager = 0;  // the link it had at the scan; one linked elsewhere since is kept
		bool mount = false;
	};
	std::vector<QueuedCreature> g_queue;
	size_t g_queue_next = 0;
	size_t g_removed = 0;
	size_t g_skipped = 0;
	bool g_auto_scan = false;            // the running scan was started by AutoCleanMinutes
	int g_seconds_to_auto = 0;
	int g_seconds_since_load = 0;
	bool g_boot_clean_done = false;
	int g_seconds_since_load_scan = 0;  // for Spawning.ScanAfterBootMinutes
	bool g_boot_scan_done = false;
	Report g_last_report;
	std::string g_last_links;
	double g_last_seconds = 0;
	Clock::time_point g_last_finished{};

	std::string ToUtf8(const FString& text)
	{
		return AsaApi::Tools::Utf8Encode(std::wstring(*text));
	}

	UWorld* World()
	{
		return AsaApi::GetApiUtils().GetWorld();
	}

	uint64_t Key(const void* pointer)
	{
		return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pointer));
	}

	ClassKind KindOf(UClass* cls)
	{
		const auto found = g_kinds.find(cls);
		if (found != g_kinds.end())
			return found->second;

		ClassKind kind;
		int depth = 0;
		for (UStruct* current = cls; current != nullptr && depth < 32; current = current->SuperStructField(), ++depth)
		{
			if (ToLower(current->NameField().ToString().ToString()) == ToLower(g_settings.rider_family))
				kind.rider = true;
		}
		if (!kind.rider)
			kind.mount = ContainsNoCase(AsaApi::IApiUtils::GetClassBlueprint(cls).ToString(), g_settings.mount_path) ||
				EndsWithNoCase(cls->NameField().ToString().ToString(), g_settings.mount_suffix);
		g_kinds.emplace(cls, kind);
		return kind;
	}

	bool IsPatrolManager(AActor* actor);
	void ReadGroups(ANPCZoneManager* manager, ManagerInfo& info);
	void ReadGroupObjects(ANPCZoneManager* manager, ManagerInfo& info);
	void ReadSpawnPoints(ANPCZoneManager* manager, ManagerInfo& info);
	void ReadPatrolPoints(ANPCZoneManager* manager, ManagerInfo& info);
	void ReadContainers(ANPCZoneManager* manager, ManagerInfo& info);

	// The manager's own bookkeeping. These fields are all in the game's symbol data; the patrol group
	// fields of the Blueprint are read by ReadGroups.
	ManagerInfo DescribeManager(ANPCZoneManager* manager)
	{
		ManagerInfo info;
		info.name = manager->NameField().ToString().ToString();
		info.linked_count = *GetNativePointerField<float*>(manager, "ANPCZoneManager.DirectLinkDinoCount");
		info.linked_list = GetNativePointerField<TArray<TWeakObjectPtr<APrimalDinoCharacter>>*>(
			manager, "ANPCZoneManager.DirectLinkedDinos")->Num();
		info.save_version = *GetNativePointerField<int*>(manager, "ANPCZoneManager.SaveForceResetPatrolsVersion");
		info.load_version = *GetNativePointerField<int*>(manager, "ANPCZoneManager.LoadForceResetPatrolsVersion");
		info.has_count = true;
		const FVector at = manager->GetLocation();
		info.x = at.X;
		info.y = at.Y;
		info.z = at.Z;
		info.enabled = *GetNativePointerField<bool*>(manager, "ANPCZoneManager.bEnabled");
		info.spawned = *GetNativePointerField<int*>(manager, "ANPCZoneManager.NumNPCSpawned");
		info.min_desired = *GetNativePointerField<int*>(manager, "ANPCZoneManager.MinDesiredNumberOfNPC");
		info.max_npc = *GetNativePointerField<int*>(manager, "ANPCZoneManager.MaxNumberOfNPC");
		info.entries = manager->NPCSpawnEntriesField().Num();
		info.has_zone = true;
		if (IsPatrolManager(manager))
		{
			try
			{
				ReadGroups(manager, info);
			}
			catch (const std::exception& error)
			{
				info.has_groups = false;
				info.groups_error = error.what();
			}
			try
			{
				ReadGroupObjects(manager, info);
			}
			catch (const std::exception& error)
			{
				info.objects_read = false;
				info.objects_error = error.what();
			}
			try
			{
				ReadSpawnPoints(manager, info);
			}
			catch (const std::exception& error)
			{
				info.spawn_read = false;
				info.spawn_checks.clear();
				info.spawn_error = error.what();
			}
			try
			{
				ReadPatrolPoints(manager, info);
			}
			catch (const std::exception& error)
			{
				info.patrol_read = false;
				info.patrol_error = error.what();
			}
			try
			{
				ReadContainers(manager, info);
			}
			catch (const std::exception& error)
			{
				info.containers_read = false;
				info.containers_error = error.what();
			}
		}
		return info;
	}

	// Patrol managers: classes deriving from NPCZoneManager whose own class name contains "Patrol"
	// (NPCZoneManager_Patrols_C). bIsPatrolManager is a bitfield, so the class name is used instead.
	std::unordered_map<UClass*, bool> g_patrol_manager_classes;

	bool IsPatrolManager(AActor* actor)
	{
		UClass* cls = reinterpret_cast<UObject*>(actor)->ClassField();
		if (cls == nullptr)
			return false;
		const auto found = g_patrol_manager_classes.find(cls);
		if (found != g_patrol_manager_classes.end())
			return found->second;
		bool is_zone = false;
		int depth = 0;
		for (UStruct* current = cls; current != nullptr && depth < 32 && !is_zone; current = current->SuperStructField(), ++depth)
			is_zone = current->NameField().ToString().ToString() == "NPCZoneManager";
		const bool is_patrol = is_zone && ContainsNoCase(cls->NameField().ToString().ToString(), "Patrol");
		g_patrol_manager_classes.emplace(cls, is_patrol);
		return is_patrol;
	}

	// Blueprint fields of a patrol manager class, found by name in the class's property list (they are not in
	// the game's symbol data). Offsets are -1 when not found; error says what is missing.
	struct PatrolLayout
	{
		std::string error;        // group counts or caps missing: nothing is read
		std::string saved_error;  // SavedGroups or its struct missing: the counts are still read
		int groups[3] = { -1, -1, -1 };
		int max_groups[3] = { -1, -1, -1 };
		int validated = -1;
		int initial_delay = -1;
		int initial_delay_size = 0;
		int patrol_groups = -1;   // -1 also when its size doesn't match the API's TMap
		int saved_groups = -1;
		int stride = 0;           // size of one Struct_PatrolGroupSavedData
		int field_id = -1, field_size = -1, field_members = -1, field_latent = -1;
		std::string spawn_error;  // spawn point lists missing: no spawn point checks
		int spawn_points[3] = { -1, -1, -1 };      // SpawnPointsSmall, ...
		int remaining_points[3] = { -1, -1, -1 };  // RemainingSpawnPointsSmall, ...
		int trying_full = -1;                      // bTryingFullIncrease
		int patrol_points = -1;                    // PatrolPoints
		int patrol_initialized = -1;               // bInitilizedPatrolPoints (sic)
	};
	std::unordered_map<UClass*, PatrolLayout> g_patrol_layouts;

	const wchar_t* const kSavedGroupStruct =
		L"/Game/LostColony/CoreBlueprints/Patrols/Structs/Struct_PatrolGroupSavedData.Struct_PatrolGroupSavedData";

	// Case-insensitive like FName (PatrolGroupManager's variable is "GroupId", the saved struct's "GroupID_7_...").
	bool SameNameStart(const std::string& found, const std::string& name, bool prefix)
	{
		if (found.size() < name.size() || (!prefix && found.size() != name.size()))
			return false;
		for (size_t i = 0; i < name.size(); ++i)
			if (std::tolower(static_cast<unsigned char>(found[i])) != std::tolower(static_cast<unsigned char>(name[i])))
				return false;
		return true;
	}

	// Blueprint struct members carry a suffix (GroupID_7_418337E0...), so those are matched by prefix.
	FProperty* FindProperty(UStruct* owner, const std::string& name, bool prefix)
	{
		int guard = 0;
		for (FProperty* property = owner->PropertyLinkField(); property != nullptr && guard < 4096;
			property = property->PropertyLinkNextField(), ++guard)
		{
			const std::string found = property->NamePrivateField().ToString().ToString();
			if (SameNameStart(found, name, prefix))
				return property;
		}
		return nullptr;
	}

	// Offset of the property if it exists with the expected size (0: any size), otherwise -1 and a note.
	int OffsetOf(UStruct* owner, const std::string& name, int size, std::string& error, bool prefix = false)
	{
		FProperty* property = FindProperty(owner, name, prefix);
		if (property == nullptr)
		{
			if (error.empty())
				error = "no " + name;
			return -1;
		}
		if (size > 0 && property->ElementSizeField() != size)
		{
			if (error.empty())
				error = name + " is " + std::to_string(property->ElementSizeField()) + " bytes, expected " + std::to_string(size);
			return -1;
		}
		return property->Offset_InternalField();
	}

	// The first names in a class or struct's property list, for a warning when an expected one is missing.
	std::string PropertyNames(UStruct* owner, int limit)
	{
		std::string names;
		int count = 0;
		for (FProperty* property = owner->PropertyLinkField(); property != nullptr && count < 4096;
			property = property->PropertyLinkNextField(), ++count)
		{
			if (count < limit)
				names += (count == 0 ? "" : ", ") + property->NamePrivateField().ToString().ToString();
		}
		if (count > limit)
			names += ", ... (" + std::to_string(count) + " in all)";
		return names.empty() ? std::string("none") : names;
	}

	const PatrolLayout& LayoutOf(UClass* cls)
	{
		const auto found = g_patrol_layouts.find(cls);
		if (found != g_patrol_layouts.end())
			return found->second;

		PatrolLayout layout;
		static const char* const kSizes[3] = { "Small", "Medium", "Large" };
		for (int i = 0; i < 3; ++i)
		{
			layout.groups[i] = OffsetOf(cls, std::string("Num") + kSizes[i] + "Groups", 4, layout.error);
			layout.max_groups[i] = OffsetOf(cls, std::string("MaxPatrolGroups") + kSizes[i], 4, layout.error);
		}
		layout.validated = OffsetOf(cls, "bValidatedNumGroups", 1, layout.error);

		// Optional fields: missing ones are left out of the report.
		if (FProperty* delay = FindProperty(cls, "InitialDelay", false);
			delay != nullptr && (delay->ElementSizeField() == 4 || delay->ElementSizeField() == 8))
		{
			layout.initial_delay = delay->Offset_InternalField();
			layout.initial_delay_size = delay->ElementSizeField();
		}
		std::string ignored;
		layout.patrol_groups = OffsetOf(cls, "PatrolGroups", static_cast<int>(sizeof(TMap<int32, UObject*>)), ignored);
		for (int i = 0; i < 3; ++i)
		{
			layout.spawn_points[i] = OffsetOf(cls, std::string("SpawnPoints") + kSizes[i],
				static_cast<int>(sizeof(TArray<AActor*>)), layout.spawn_error);
			layout.remaining_points[i] = OffsetOf(cls, std::string("RemainingSpawnPoints") + kSizes[i],
				static_cast<int>(sizeof(TArray<AActor*>)), layout.spawn_error);
		}
		layout.trying_full = OffsetOf(cls, "bTryingFullIncrease", 1, ignored);
		layout.patrol_points = OffsetOf(cls, "PatrolPoints", static_cast<int>(sizeof(TArray<AActor*>)), ignored);
		layout.patrol_initialized = OffsetOf(cls, "bInitilizedPatrolPoints", 1, ignored);

		layout.saved_groups = OffsetOf(cls, "SavedGroups", static_cast<int>(sizeof(TArray<uint8>)), layout.saved_error);
		auto* entry = static_cast<UScriptStruct*>(Globals::StaticFindObject(UScriptStruct::StaticClass(), nullptr, kSavedGroupStruct, false));
		if (entry == nullptr)
		{
			if (layout.saved_error.empty())
				layout.saved_error = "Struct_PatrolGroupSavedData not found";
		}
		else
		{
			const int align = std::max<int>(1, entry->MinAlignmentField());
			layout.stride = (entry->PropertiesSizeField() + align - 1) / align * align;
			layout.field_id = OffsetOf(entry, "GroupID_", 4, layout.saved_error, true);
			layout.field_size = OffsetOf(entry, "GroupSize_", 4, layout.saved_error, true);
			layout.field_members = OffsetOf(entry, "NumMembers_", 4, layout.saved_error, true);
			layout.field_latent = OffsetOf(entry, "LatentSpawnCount_", 4, layout.saved_error, true);
			if (layout.saved_error.empty() && layout.stride <= 0)
				layout.saved_error = "Struct_PatrolGroupSavedData has size 0";
		}

		if (!layout.error.empty())
			PluginLog()->warn("Patrol groups of {} not readable: {}", cls->NameField().ToString().ToString(), layout.error);
		else if (!layout.saved_error.empty())
			PluginLog()->warn("Saved patrol groups of {} not readable: {}", cls->NameField().ToString().ToString(), layout.saved_error);
		if (!layout.spawn_error.empty())
			PluginLog()->warn("Patrol spawn points of {} not readable: {}", cls->NameField().ToString().ToString(), layout.spawn_error);
		return g_patrol_layouts.emplace(cls, layout).first->second;
	}

	// Blueprint fields of a patrol group object class (PatrolGroupManager_C), found by name like the
	// manager's.
	struct GroupLayout
	{
		std::string error;  // a required field is missing: no group object of this class is read
		int id = -1, size = -1, num_members = -1, latent = -1, members = -1;
		int nav = -1;       // LastTimeUpdatedNavigationTargetPoint (optional)
		int nav_size = 0;
		int path = -1;      // bDidGeneratePath (optional)
	};
	std::unordered_map<UClass*, GroupLayout> g_group_layouts;

	const GroupLayout& GroupLayoutOf(UClass* cls)
	{
		const auto found = g_group_layouts.find(cls);
		if (found != g_group_layouts.end())
			return found->second;

		GroupLayout layout;
		layout.id = OffsetOf(cls, "GroupId", 4, layout.error);
		layout.size = OffsetOf(cls, "GroupSize", 4, layout.error);
		layout.num_members = OffsetOf(cls, "NumMembers", 4, layout.error);
		layout.latent = OffsetOf(cls, "LatentSpawnCount", 4, layout.error);
		layout.members = OffsetOf(cls, "GroupMembers", static_cast<int>(sizeof(TArray<APrimalDinoCharacter*>)), layout.error);
		if (FProperty* nav = FindProperty(cls, "LastTimeUpdatedNavigationTargetPoint", false);
			nav != nullptr && (nav->ElementSizeField() == 4 || nav->ElementSizeField() == 8))
		{
			layout.nav = nav->Offset_InternalField();
			layout.nav_size = nav->ElementSizeField();
		}
		std::string ignored;
		layout.path = OffsetOf(cls, "bDidGeneratePath", 1, ignored);
		if (!layout.error.empty())
		{
			const std::string name = AsaApi::IApiUtils::GetClassBlueprint(cls).ToString();
			layout.error += " on " + name;
			PluginLog()->warn("Patrol group objects of {} not readable: {}. Its properties: {}", name, layout.error,
				PropertyNames(cls, 80));
		}
		return g_group_layouts.emplace(cls, layout).first->second;
	}

	double NetworkTime()
	{
		UWorld* world = World();
		return world != nullptr ? UVictoryCore::GetNetworkTimeInSeconds(world) : 0.0;
	}

	// The group objects in the manager's PatrolGroups, with their own counts (not SavedGroups').
	void ReadGroupObjects(ANPCZoneManager* manager, ManagerInfo& info)
	{
		const PatrolLayout& layout = LayoutOf(manager->ClassField());
		if (layout.patrol_groups < 0)
		{
			info.objects_error = "PatrolGroups not found or not the size of a TMap<int32, UObject*>";
			return;
		}
		auto& groups = *reinterpret_cast<TMap<int32, UObject*>*>(reinterpret_cast<uint8_t*>(manager) + layout.patrol_groups);
		const int count = groups.Num();
		if (count < 0 || count > 10000)
		{
			info.objects_error = "PatrolGroups looks invalid (" + std::to_string(count) + " entries)";
			return;
		}
		const double now = NetworkTime();
		const auto read_int = [](const uint8_t* at) { return *reinterpret_cast<const int32*>(at); };
		for (auto& pair : groups)
			info.group_keys.push_back(pair.Key);
		for (auto& pair : groups)
		{
			UObject* object = pair.Value;
			if (object == nullptr || !object->IsValidLowLevel() || object->ClassField() == nullptr)
				continue;
			const GroupLayout& group_layout = GroupLayoutOf(object->ClassField());
			if (!group_layout.error.empty())
			{
				info.objects_error = group_layout.error;
				info.objects.clear();
				return;
			}
			const auto* base = reinterpret_cast<const uint8_t*>(object);
			GroupObject group;
			group.key = pair.Key;
			group.id = read_int(base + group_layout.id);
			group.size = read_int(base + group_layout.size);
			group.num_members = read_int(base + group_layout.num_members);
			group.latent = read_int(base + group_layout.latent);
			const auto& members = *reinterpret_cast<const TArray<APrimalDinoCharacter*>*>(base + group_layout.members);
			group.listed = members.Num();
			if (group.listed > 0 && group.listed <= 1000)
			{
				for (int32 i = 0; i < group.listed; ++i)
				{
					APrimalDinoCharacter* dino = members[i];
					if (dino == nullptr || !dino->IsValidLowLevel())
						continue;
					++group.valid;
					if (!dino->bStasised()())
						++group.awake;
				}
			}
			if (group_layout.nav >= 0)
			{
				const double then = group_layout.nav_size == 8 ? *reinterpret_cast<const double*>(base + group_layout.nav)
					: *reinterpret_cast<const float*>(base + group_layout.nav);
				group.nav_read = true;
				group.nav_never = then == 0.0;
				group.nav_age = now - then;
			}
			group.path = group_layout.path < 0 || base[group_layout.path] != 0;
			for (size_t i = 0; i < info.saved.size(); ++i)
				if (info.saved[i].id == group.id)
				{
					group.saved_index = static_cast<int>(i);
					break;
				}
			info.objects.push_back(group);
		}
		std::sort(info.objects.begin(), info.objects.end(), [](const GroupObject& a, const GroupObject& b) { return a.id < b.id; });
		info.objects_read = true;
	}

	// Spawn point counts, and for each size below its cap the tests FindSpawnPoint makes on every point.
	void ReadSpawnPoints(ANPCZoneManager* manager, ManagerInfo& info)
	{
		const PatrolLayout& layout = LayoutOf(manager->ClassField());
		if (!layout.spawn_error.empty())
		{
			info.spawn_error = layout.spawn_error;
			return;
		}
		auto* base = reinterpret_cast<uint8_t*>(manager);
		const auto list = [base](int offset) -> TArray<AActor*>& { return *reinterpret_cast<TArray<AActor*>*>(base + offset); };
		for (int i = 0; i < 3; ++i)
		{
			info.points[i] = list(layout.spawn_points[i]).Num();
			info.remaining[i] = list(layout.remaining_points[i]).Num();
		}
		info.trying_full = layout.trying_full >= 0 && base[layout.trying_full] != 0;
		info.try_interval = manager->MinimumManualSpawnIntervalField();
		if (const double last = manager->LastManuallySpawnedTimeField(); last != 0.0)
			info.since_try = NetworkTime() - last;
		info.spawn_read = true;
		if (!info.has_groups)
			return;

		UWorld* world = World();
		for (int size = 0; size < 3; ++size)
		{
			if (info.groups[size] >= info.max_groups[size])
				continue;
			TArray<AActor*>& points = list(layout.spawn_points[size]);
			TArray<AActor*>& remaining = list(layout.remaining_points[size]);
			if (points.Num() <= 0 || points.Num() > 1000 || remaining.Num() < 0 || remaining.Num() > 1000)
				continue;
			const float radius = static_cast<float>(ObstructionRadius(size));
			for (int32 i = 0; i < points.Num(); ++i)
			{
				AActor* point = points[i];
				if (point == nullptr || !point->IsValidLowLevel())
					continue;
				SpawnPointCheck check;
				check.size = size;
				check.name = point->NameField().ToString().ToString();
				FVector at = point->GetLocation();
				check.x = at.X;
				check.y = at.Y;
				check.z = at.Z;
				for (int32 j = 0; j < remaining.Num(); ++j)
					if (remaining[j] == point)
						check.remaining = true;
				check.players = manager->MinimumPlayerDistanceCheck(&at, 1.0f);
				check.tames = manager->MinimumTamedDinoDistanceCheck(&at, 1.0f);
				check.structures = manager->MinimumStructureDistanceCheck(&at, 1.0f);
				if (world != nullptr)
				{
					TArray<AActor*> nearby;
					UVictoryCore::ServerOctreeOverlapActors(&nearby, world, &at, radius, EServerOctreeGroup::DINOPAWNS, false);
					double closest = 0;
					for (AActor* dino : nearby)
					{
						if (dino == nullptr)
							continue;
						++check.dinos;
						const FVector where = dino->GetLocation();
						const double dx = where.X - at.X, dy = where.Y - at.Y, dz = where.Z - at.Z;
						const double distance = dx * dx + dy * dy + dz * dz;
						if (check.nearest.empty() || distance < closest)
						{
							closest = distance;
							check.nearest = dino->ClassField() != nullptr ? dino->ClassField()->NameField().ToString().ToString() : "?";
						}
					}
				}
				info.spawn_checks.push_back(check);
			}
		}
	}

	void ReadGroups(ANPCZoneManager* manager, ManagerInfo& info)
	{
		info.manual = manager->bUsesManualSpawningField();
		const PatrolLayout& layout = LayoutOf(manager->ClassField());
		if (!layout.error.empty())
		{
			info.groups_error = layout.error;
			return;
		}

		auto* base = reinterpret_cast<uint8_t*>(manager);
		const auto read_int = [](const uint8_t* at) { return *reinterpret_cast<const int32*>(at); };
		for (int i = 0; i < 3; ++i)
		{
			info.groups[i] = read_int(base + layout.groups[i]);
			info.max_groups[i] = read_int(base + layout.max_groups[i]);
		}
		info.validated = base[layout.validated] != 0;
		if (layout.initial_delay >= 0)
			info.initial_delay = layout.initial_delay_size == 8 ? *reinterpret_cast<const double*>(base + layout.initial_delay)
				: *reinterpret_cast<const float*>(base + layout.initial_delay);
		if (layout.patrol_groups >= 0)
			info.live_groups = reinterpret_cast<TMap<int32, UObject*>*>(base + layout.patrol_groups)->Num();
		info.has_groups = true;

		if (!layout.saved_error.empty())
		{
			info.groups_error = layout.saved_error;
			return;
		}
		auto& saved = *reinterpret_cast<TArray<uint8>*>(base + layout.saved_groups);
		const int count = saved.Num();
		const uint8_t* data = saved.GetData();
		if (count < 0 || count > 10000 || (count > 0 && data == nullptr))
		{
			info.groups_error = "SavedGroups looks invalid (" + std::to_string(count) + " entries)";
			return;
		}
		for (int i = 0; i < count; ++i)
		{
			const uint8_t* at = data + static_cast<size_t>(i) * layout.stride;
			SavedGroup group;
			group.id = read_int(at + layout.field_id);
			group.size = read_int(at + layout.field_size);
			group.members = read_int(at + layout.field_members);
			group.latent = read_int(at + layout.field_latent);
			info.saved.push_back(group);
		}
		info.saved_read = true;
	}

	// Dissolves saved groups through the manager's own OnGroupDissolved(GroupId) Blueprint event, which
	// removes the SavedGroups entry and counts the group's size down (only when its LatentSpawnCount is 0).
	// Returns how many calls were made.
	int DissolveGroups(ANPCZoneManager* manager, const std::vector<int>& ids)
	{
		UFunction* function = manager->ClassField()->FindFunctionByName("OnGroupDissolved", EIncludeSuperFlag::IncludeSuper);
		if (function == nullptr)
			throw std::runtime_error("no OnGroupDissolved on " + manager->ClassField()->NameField().ToString().ToString());
		std::string error;
		const int id_offset = OffsetOf(function, "GroupID", 4, error);  // FindProperty ignores case
		if (!error.empty())
			throw std::runtime_error("OnGroupDissolved: " + error);
		const int size = function->PropertiesSizeField();
		if (size < id_offset + 4 || size > 4096)
			throw std::runtime_error("OnGroupDissolved: unexpected parameter size " + std::to_string(size));
		std::vector<uint64_t> params((static_cast<size_t>(size) + 7) / 8 + 1);
		int calls = 0;
		for (const int id : ids)
		{
			std::fill(params.begin(), params.end(), 0);
			*reinterpret_cast<int32*>(reinterpret_cast<uint8_t*>(params.data()) + id_offset) = id;
			manager->ProcessEvent(function, params.data());
			++calls;
		}
		return calls;
	}

	// Connections of APrimalPatrolPoint (native): found by name, like the Blueprint fields.
	struct ConnectionLayout
	{
		std::string error;
		int connections = -1;  // APrimalPatrolPoint.Connections
		int stride = 0;        // size of one FPatrolPointConnection
		int occupied = -1;     // OccupiedByGroupID
		int path_size = -1;    // PathSize
	};
	std::unordered_map<UClass*, ConnectionLayout> g_connection_layouts;

	const ConnectionLayout& ConnectionLayoutOf(UClass* cls)
	{
		const auto found = g_connection_layouts.find(cls);
		if (found != g_connection_layouts.end())
			return found->second;
		ConnectionLayout layout;
		layout.connections = OffsetOf(cls, "Connections", static_cast<int>(sizeof(TArray<uint8>)), layout.error);
		auto* entry = static_cast<UScriptStruct*>(Globals::StaticFindObject(UScriptStruct::StaticClass(), nullptr,
			L"/Script/ShooterGame.PatrolPointConnection", false));
		if (entry == nullptr)
		{
			if (layout.error.empty())
				layout.error = "PatrolPointConnection not found";
		}
		else
		{
			const int align = (std::max<int>)(1, entry->MinAlignmentField());
			layout.stride = (entry->PropertiesSizeField() + align - 1) / align * align;
			layout.occupied = OffsetOf(entry, "OccupiedByGroupID", 4, layout.error);
			layout.path_size = OffsetOf(entry, "PathSize", 4, layout.error);
			if (layout.error.empty() && layout.stride <= 0)
				layout.error = "PatrolPointConnection has size 0";
		}
		if (!layout.error.empty())
			PluginLog()->warn("Patrol point connections of {} not readable: {}", cls->NameField().ToString().ToString(), layout.error);
		return g_connection_layouts.emplace(cls, layout).first->second;
	}

	// The manager's patrol points and who occupies their connections. FindNearestPatrolPoint takes the
	// nearest point with a connection of PathSize >= the group size and OccupiedByGroupID <= 0.
	void ReadPatrolPoints(ANPCZoneManager* manager, ManagerInfo& info)
	{
		const PatrolLayout& layout = LayoutOf(manager->ClassField());
		if (layout.patrol_points < 0)
		{
			info.patrol_error = "no PatrolPoints";
			return;
		}
		auto* base = reinterpret_cast<uint8_t*>(manager);
		auto& points = *reinterpret_cast<TArray<AActor*>*>(base + layout.patrol_points);
		info.patrol_points = points.Num();
		info.patrol_initialized = layout.patrol_initialized >= 0 && base[layout.patrol_initialized] != 0;
		if (info.patrol_points < 0 || info.patrol_points > 100000)
		{
			info.patrol_error = "PatrolPoints looks invalid (" + std::to_string(info.patrol_points) + " entries)";
			return;
		}
		std::map<int, int> occupied;
		for (int32 i = 0; i < points.Num(); ++i)
		{
			AActor* point = points[i];
			if (point == nullptr || !point->IsValidLowLevel() || point->ClassField() == nullptr)
				continue;
			const ConnectionLayout& connection = ConnectionLayoutOf(point->ClassField());
			if (!connection.error.empty())
			{
				info.patrol_error = connection.error;
				return;
			}
			auto& list = *reinterpret_cast<TArray<uint8>*>(reinterpret_cast<uint8_t*>(point) + connection.connections);
			const int count = list.Num();
			const uint8_t* data = list.GetData();
			if (count <= 0 || count > 1000 || data == nullptr)
				continue;
			bool has_free[3] = {};
			for (int j = 0; j < count; ++j)
			{
				const uint8_t* at = data + static_cast<size_t>(j) * connection.stride;
				const int by = *reinterpret_cast<const int32*>(at + connection.occupied);
				const int path_size = *reinterpret_cast<const int32*>(at + connection.path_size);
				++info.connections;
				if (by > 0)
				{
					++occupied[by];
					continue;
				}
				for (int size = 0; size < 3; ++size)
					if (path_size >= size)
					{
						++info.free_connections[size];
						has_free[size] = true;
					}
			}
			for (int size = 0; size < 3; ++size)
				info.points_with_free[size] += has_free[size] ? 1 : 0;
		}
		info.occupied.assign(occupied.begin(), occupied.end());
		std::sort(info.occupied.begin(), info.occupied.end(), [](const auto& a, const auto& b)
			{ return a.second != b.second ? a.second > b.second : a.first < b.first; });
		info.patrol_read = true;
	}

	// ------------------------------------------------------------------------------------------
	// SpawnPatrolGroup trace and spawn entries restore
	// ------------------------------------------------------------------------------------------

	// The patrol Blueprint's DoSpawnPatrolGroup hands each new group to the native
	// ANPCZoneManager::SpawnPatrolGroup with the manager's NPCSpawnEntriesGroupSmall/Medium/Large container
	// class. The trace logs the first calls per manager per load (then every kTraceEvery-th, and any call whose
	// SpawnChecks differ from the manager's previous call) with how many
	// creatures each call spawned (the manager's NumNPCSpawned before and after), and each container's
	// spawn entries once, with every creature class and whether it is loaded.
	constexpr const char* kSpawnPatrolGroupFunc =
		"ANPCZoneManager.SpawnPatrolGroup(TSubclassOf<UNPCSpawnEntriesContainer>,UE::Math::TVector<double>,UE::Math::TRotator<double>,int,int)";
	constexpr int kTraceFirstCalls = 3;
	constexpr int kTraceEvery = 100;
	struct SpawnTrace
	{
		int calls = 0;
		int spawned = 0;
		std::string checks; // SpawnChecks of the previous call; a change is always logged
	};
	std::unordered_map<uint64_t, SpawnTrace> g_spawn_traces;
	std::unordered_set<UClass*> g_containers_logged;
	bool g_spawn_hooked = false;

	std::string SoftPath(const TSoftClassPtr<APrimalDinoCharacter>& soft)
	{
		FString text;
		const_cast<FSoftObjectPath&>(soft.ToSoftObjectPath()).ToString(&text);
		return ToUtf8(text);
	}

	std::string ClassName(UClass* cls)
	{
		if (cls == nullptr)
			return "none";
		return cls->IsValidLowLevel() ? cls->NameField().ToString().ToString() : std::string("invalid");
	}

	void LogContainer(UClass* cls)
	{
		if (cls == nullptr || !cls->IsValidLowLevel() || !g_containers_logged.insert(cls).second)
			return;
		const std::string name = ClassName(cls);
		auto* container = static_cast<UNPCSpawnEntriesContainer*>(cls->ClassDefaultObjectField());
		if (container == nullptr)
		{
			PluginLog()->warn("SpawnPatrolGroup container {}: no default object", name);
			return;
		}
		auto* entry_struct = static_cast<UScriptStruct*>(Globals::StaticFindObject(UScriptStruct::StaticClass(), nullptr,
			L"/Script/ShooterGame.NPCSpawnEntry", false));
		if (entry_struct == nullptr || entry_struct->PropertiesSizeField() != static_cast<int>(sizeof(FNPCSpawnEntry)))
		{
			PluginLog()->warn("SpawnPatrolGroup container {}: {} entries; FNPCSpawnEntry layout differs from the API header, not listed",
				name, container->NPCSpawnEntriesField().Num());
			return;
		}
		const auto& entries = container->NPCSpawnEntriesField();
		std::string text;
		int classes = 0;
		int loaded = 0;
		for (int32 i = 0; i < entries.Num(); ++i)
		{
			const FNPCSpawnEntry& entry = entries[i];
			text += "\n    " + ToUtf8(entry.AnEntryName) + ": ";
			if (entry.NPCsToSpawn.Num() == 0)
				text += "no classes";
			for (int32 j = 0; j < entry.NPCsToSpawn.Num(); ++j)
			{
				const auto& soft = entry.NPCsToSpawn[j];
				const bool is_loaded = soft.Get() != nullptr;
				++classes;
				loaded += is_loaded ? 1 : 0;
				text += (j == 0 ? "" : ", ") + SoftPath(soft) + (is_loaded ? "" : " (NOT LOADED)");
			}
		}
		PluginLog()->info("SpawnPatrolGroup container {}: {} entries, {} of {} creature classes loaded{}", name, entries.Num(),
			loaded, classes, text);
	}

	// NPCSpawnEntriesGroupSmall/Medium/Large of a patrol manager class (TSubclassOf<UNPCSpawnEntriesContainer>,
	// one class pointer each), found by name like the other Blueprint fields.
	struct ContainerLayout
	{
		std::string error;
		int offset[3] = { -1, -1, -1 };
	};
	std::unordered_map<UClass*, ContainerLayout> g_container_layouts;

	const ContainerLayout& ContainerLayoutOf(UClass* cls)
	{
		const auto found = g_container_layouts.find(cls);
		if (found != g_container_layouts.end())
			return found->second;
		ContainerLayout layout;
		static const char* const kSizes[3] = { "Small", "Medium", "Large" };
		for (int i = 0; i < 3; ++i)
			layout.offset[i] = OffsetOf(cls, std::string("NPCSpawnEntriesGroup") + kSizes[i], static_cast<int>(sizeof(UClass*)), layout.error);
		if (!layout.error.empty())
			PluginLog()->warn("Patrol spawn entries of {} not readable: {}", cls->NameField().ToString().ToString(), layout.error);
		return g_container_layouts.emplace(cls, layout).first->second;
	}

	UClass*& ContainerAt(UObject* object, int offset)
	{
		return *reinterpret_cast<UClass**>(reinterpret_cast<uint8_t*>(object) + offset);
	}

	void ReadContainers(ANPCZoneManager* manager, ManagerInfo& info)
	{
		UClass* cls = manager->ClassField();
		const ContainerLayout& layout = ContainerLayoutOf(cls);
		if (!layout.error.empty())
		{
			info.containers_error = layout.error;
			return;
		}
		UObject* defaults = cls->ClassDefaultObjectField();
		for (int i = 0; i < 3; ++i)
		{
			info.containers[i] = ClassName(ContainerAt(manager, layout.offset[i]));
			info.default_containers[i] = defaults != nullptr ? ClassName(ContainerAt(defaults, layout.offset[i]))
				: std::string("no default object");
		}
		info.containers_read = true;
	}

	// Size (0 small, 1 medium, 2 large) of the saved group with this id, or -1. DoSpawnPatrolGroup adds the
	// group's SavedGroups entry before it calls SpawnPatrolGroup.
	int SavedGroupSize(ANPCZoneManager* manager, int group_id)
	{
		const PatrolLayout& layout = LayoutOf(manager->ClassField());
		if (!layout.saved_error.empty() || layout.saved_groups < 0 || layout.stride <= 0)
			return -1;
		auto& saved = *reinterpret_cast<TArray<uint8>*>(reinterpret_cast<uint8_t*>(manager) + layout.saved_groups);
		const int count = saved.Num();
		const uint8_t* data = saved.GetData();
		if (count <= 0 || count > 10000 || data == nullptr)
			return -1;
		for (int i = 0; i < count; ++i)
		{
			const uint8_t* at = data + static_cast<size_t>(i) * layout.stride;
			if (*reinterpret_cast<const int32*>(at + layout.field_id) == group_id)
				return *reinterpret_cast<const int32*>(at + layout.field_size);
		}
		return -1;
	}

	std::unordered_set<uint64_t> g_restore_logged;  // managers whose restore was logged this load

	// Sets the manager's empty NPCSpawnEntriesGroup* fields from its class default and returns the container
	// for this group's size (nullptr when there is none to give).
	UClass* RestoreContainers(ANPCZoneManager* manager, int group_id)
	{
		UClass* cls = manager->ClassField();
		const ContainerLayout& layout = ContainerLayoutOf(cls);
		UObject* defaults = cls->ClassDefaultObjectField();
		if (!layout.error.empty() || defaults == nullptr)
			return nullptr;
		std::string changed;
		for (int i = 0; i < 3; ++i)
		{
			UClass*& field = ContainerAt(manager, layout.offset[i]);
			UClass* fallback = ContainerAt(defaults, layout.offset[i]);
			if (field == nullptr && fallback != nullptr)
			{
				field = fallback;
				changed += (changed.empty() ? "" : ", ") + GroupSizeName(i) + " " + ClassName(fallback);
			}
		}
		const int size = SavedGroupSize(manager, group_id);
		UClass* result = size >= 0 && size < 3 ? ContainerAt(manager, layout.offset[size]) : nullptr;
		if (g_restore_logged.insert(Key(manager)).second)
		{
			const std::string name = manager->NameField().ToString().ToString();
			if (!changed.empty())
				PluginLog()->info("Spawn entries of {} were empty; set from the class default: {}", name, changed);
			else
				PluginLog()->warn("SpawnPatrolGroup on {} got no container, but the manager's spawn entries are not empty "
					"(or the class default is empty too)", name);
			if (result == nullptr)
				PluginLog()->warn("SpawnPatrolGroup on {}: size of group #{} not found ({}); this group goes out as the game sent it, "
					"the next ones read the restored fields", name, group_id, size);
		}
		return result;
	}

	// Inner calls of one SpawnPatrolGroup call (log only): which of the game's spawn steps it reaches and
	// what each returns. Only counted while a SpawnPatrolGroup call is running (all on the game thread).
	// IsValidNPCEntry is also hooked by other plugins; a refusal by one of them may not reach this hook.
	constexpr const char* kIsValidEntryFunc = "ANPCZoneManager.IsValidNPCEntry(FNPCSpawnEntry&)";
	constexpr const char* kSpawnFromEntriesFunc =
		"ANPCZoneManager.SpawnFromEntries(int,TArray<FNPCSpawnEntry,TSizedDefaultAllocator<32>>&,UE::Math::TVector<double>&,"
		"TArray<APrimalDinoCharacter*,TSizedDefaultAllocator<32>>*,float)";
	constexpr const char* kSpawnNpcFunc =
		"ANPCZoneManager.SpawnNPC(bool,bool&,TSoftClassPtr<APrimalDinoCharacter>,UE::Math::TVector<double>,bool,int,float,bool,float,float,float,float,TSubclassOf<UPrimalColorSet>,UE::Math::TVector<double>,TSubclassOf<UPrimalEscortComponent>)";
	constexpr const char* kInjectLatentFunc =
		"AHibernationManager.InjectLatentSpawn(FLatentSpawnParams&,ANPCZoneManager*,FPrimalDinoDBEntry*)";
	struct InnerTrace
	{
		bool active = false;
		int valid_calls = 0, valid_refused = 0;
		int from_entries = 0, from_entries_null = 0, from_entries_creatures = 0;
		int from_entries_volume = 0, from_entries_count = -1;
		int spawn_npc = 0, spawn_npc_null = 0, spawn_npc_flag = 0;
		std::string first_npc;  // the first SpawnNPC call: class, where, its bool arguments and result
		int latent = 0;
	};
	InnerTrace g_inner;
	bool g_inner_hooked[4] = {};

	std::string FormatInner(const InnerTrace& t)
	{
		std::ostringstream out;
		out << "inside: IsValidNPCEntry " << t.valid_calls << " (refused " << t.valid_refused << "), SpawnFromEntries "
			<< t.from_entries;
		if (t.from_entries > 0)
			out << " (volume " << t.from_entries_volume << ", " << t.from_entries_count << " entries, returned none "
				<< t.from_entries_null << ", " << t.from_entries_creatures << " creatures listed)";
		out << ", SpawnNPC " << t.spawn_npc;
		if (t.spawn_npc > 0)
			out << " (returned none " << t.spawn_npc_null << ", out flag set " << t.spawn_npc_flag << "; first: " << t.first_npc << ")";
		out << ", InjectLatentSpawn " << t.latent;
		return out.str();
	}

	DECLARE_HOOK(ANPCZoneManager_IsValidNPCEntry, bool, ANPCZoneManager* manager, FNPCSpawnEntry* entry)
	{
		const bool result = ANPCZoneManager_IsValidNPCEntry_original(manager, entry);
		if (g_inner.active)
		{
			++g_inner.valid_calls;
			g_inner.valid_refused += result ? 0 : 1;
		}
		return result;
	}

	DECLARE_HOOK(ANPCZoneManager_SpawnFromEntries, APrimalDinoCharacter*, ANPCZoneManager* manager, int volume_entry,
		TArray<FNPCSpawnEntry>* entries, FVector* at, TArray<APrimalDinoCharacter*>* all_spawned, float yaw)
	{
		const int before = all_spawned != nullptr ? all_spawned->Num() : 0;
		APrimalDinoCharacter* result = ANPCZoneManager_SpawnFromEntries_original(manager, volume_entry, entries, at, all_spawned, yaw);
		if (g_inner.active)
		{
			if (g_inner.from_entries == 0)
			{
				g_inner.from_entries_volume = volume_entry;
				g_inner.from_entries_count = entries != nullptr ? entries->Num() : -1;
			}
			++g_inner.from_entries;
			g_inner.from_entries_null += result == nullptr ? 1 : 0;
			if (all_spawned != nullptr)
				g_inner.from_entries_creatures += all_spawned->Num() - before;
		}
		return result;
	}

	// By-value TSoftClassPtr, vectors and TSubclassOf arrive as pointers to copies; the returned creature is
	// passed through whatever it is.
	DECLARE_HOOK(ANPCZoneManager_SpawnNPC, APrimalDinoCharacter*, ANPCZoneManager* manager, bool first_flag, bool* out_flag,
		TSoftClassPtr<APrimalDinoCharacter>* npc_class, FVector* at, bool override_level, int level_offset, float level_multiplier,
		bool level_offset_first, float water_min, float extent_z, float yaw, float water_max, void* colors, FVector* offset, void* escort)
	{
		APrimalDinoCharacter* result = ANPCZoneManager_SpawnNPC_original(manager, first_flag, out_flag, npc_class, at, override_level,
			level_offset, level_multiplier, level_offset_first, water_min, extent_z, yaw, water_max, colors, offset, escort);
		if (g_inner.active)
		{
			const bool flag = out_flag != nullptr && *out_flag;
			if (g_inner.spawn_npc == 0)
			{
				try
				{
					char where[96];
					std::snprintf(where, sizeof(where), "%.0f %.0f %.0f", at != nullptr ? at->X : 0.0, at != nullptr ? at->Y : 0.0,
						at != nullptr ? at->Z : 0.0);
					g_inner.first_npc = (npc_class != nullptr ? SoftPath(*npc_class) : std::string("no class")) + " at " + where +
						", first argument " + (first_flag ? "true" : "false") + ", out flag " + (flag ? "true" : "false") + ", " +
						(result != nullptr ? "spawned" : "returned none");
				}
				catch (const std::exception& error)
				{
					g_inner.first_npc = std::string("not described: ") + error.what();
				}
			}
			++g_inner.spawn_npc;
			g_inner.spawn_npc_null += result == nullptr ? 1 : 0;
			g_inner.spawn_npc_flag += flag ? 1 : 0;
		}
		return result;
	}

	DECLARE_HOOK(AHibernationManager_InjectLatentSpawn, void, void* hibernation, void* params, ANPCZoneManager* zone, void* entry)
	{
		if (g_inner.active)
			++g_inner.latent;
		AHibernationManager_InjectLatentSpawn_original(hibernation, params, zone, entry);
	}

	// The game's SpawnPatrolGroup (98.6, read from the server binary) returns without spawning unless the
	// container is set, bUsesManualSpawning is true, the group id is above 0 and, on Lost Colony
	// (GIsLostColonyMap), the world settings' player controller list is not empty. These are read before the
	// call and logged with it.
	std::string SpawnChecks(ANPCZoneManager* manager)
	{
		std::string text = std::string("manual ") + (manager->bUsesManualSpawningField() ? "yes" : "NO");
		try
		{
			const bool* lost_colony = GetNativeDataPointerField<bool*>("Global.GIsLostColonyMap");
			text += std::string(", Lost Colony ") + (lost_colony == nullptr ? "?" : (*lost_colony ? "yes" : "no"));
		}
		catch (const std::exception&)
		{
			text += ", Lost Colony ?";
		}
		UWorld* world = World();
		AWorldSettings* settings = world != nullptr ? world->GetWorldSettings(false, true) : nullptr;
		if (settings == nullptr)
			return text + ", no world settings";
		const int controllers = settings->GetActorListCount(AL_PLAYERCONTROLLERS);
		return text + ", player controllers " + std::to_string(controllers);
	}

	// The by-value TSubclassOf, vector and rotator all arrive as pointers to the caller's copies. AsaApi's own
	// wrappers take by-value TSubclassOf arguments the same way; reading this one as a class pointer showed
	// the address of the copy, never a class.
	DECLARE_HOOK(ANPCZoneManager_SpawnPatrolGroup, void, ANPCZoneManager* manager,
		TSubclassOf<UNPCSpawnEntriesContainer>* container_arg, UE::Math::TVector<double>* at,
		UE::Math::TRotator<double>* rotation, int volume_index, int group_id)
	{
		UClass* container = container_arg != nullptr ? container_arg->uClass : nullptr;
		int before = 0;
		const bool readable = manager != nullptr && manager->IsValidLowLevel();
		if (readable)
			before = manager->NumNPCSpawnedField();
		bool restored = false;
		if (readable && container_arg != nullptr && container == nullptr && g_settings.restore_spawn_entries)
		{
			try
			{
				if (UClass* replacement = RestoreContainers(manager, group_id); replacement != nullptr)
				{
					container_arg->uClass = replacement;
					container = replacement;
					restored = true;
				}
			}
			catch (const std::exception& error)
			{
				PluginLog()->warn("Spawn entries restore failed: {}", error.what());
			}
		}
		std::string checks;
		if (readable)
		{
			try
			{
				checks = SpawnChecks(manager);
			}
			catch (const std::exception& error)
			{
				checks = std::string("checks not read: ") + error.what();
			}
		}
		g_inner = InnerTrace{};
		g_inner.active = true;
		ANPCZoneManager_SpawnPatrolGroup_original(manager, container_arg, at, rotation, volume_index, group_id);
		g_inner.active = false;
		if (!readable)
			return;
		try
		{
			const int spawned = manager->NumNPCSpawnedField() - before;
			SpawnTrace& trace = g_spawn_traces[Key(manager)];
			++trace.calls;
			trace.spawned += spawned > 0 ? spawned : 0;
			const bool first_success = spawned > 0 && trace.spawned == spawned;
			const bool checks_changed = trace.calls > 1 && checks != trace.checks;
			trace.checks = checks;
			if (trace.calls > kTraceFirstCalls && trace.calls % kTraceEvery != 0 && !first_success && !checks_changed)
				return;
			LogContainer(container);
			PluginLog()->info(
				"SpawnPatrolGroup on {}: group #{}, container {}, at {:.0f} {:.0f} {:.0f}, volume index {}, spawning id after {}: "
				"{} spawned (call {} since load, {} spawned in all); {}; {}",
				manager->NameField().ToString().ToString(), group_id,
				container_arg == nullptr ? std::string("argument missing") : ClassName(container) + (restored ? " (restored)" : ""),
				at != nullptr ? at->X : 0.0, at != nullptr ? at->Y : 0.0, at != nullptr ? at->Z : 0.0, volume_index,
				manager->SpawningPatrolGroupIDField(), spawned, trace.calls, trace.spawned, checks, FormatInner(g_inner));
		}
		catch (const std::exception& error)
		{
			PluginLog()->warn("SpawnPatrolGroup trace failed: {}", error.what());
		}
	}

	// ------------------------------------------------------------------------------------------
	// Config
	// ------------------------------------------------------------------------------------------

	bool ReloadSettings(std::string& error)
	{
		try
		{
			const std::string path = AsaApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/" PROJECT_NAME "/config.json";
			std::ifstream file(path);
			if (!file.is_open())
				throw std::runtime_error("cannot open " + path);

			nlohmann::json root;
			try
			{
				// Last argument: allow // and /* */ comments in config.json.
				root = nlohmann::json::parse(file, nullptr, true, true);
			}
			catch (const nlohmann::json::exception& json_error)
			{
				throw std::runtime_error(std::string("config.json is not valid JSON: ") + json_error.what());
			}

			Settings settings = ParseSettings(root);
			g_logging_enabled = settings.logging; // before the warnings below, so they honour it too
			for (const std::string& warning : settings.warnings)
				PluginLog()->warn("Config: {}", warning);
			g_settings = std::move(settings);
			g_settings_loaded = true;
			g_seconds_to_auto = g_settings.auto_clean_minutes * 60;  // first automatic clean one interval from now
			g_kinds.clear(); // the family or path may have changed
			return true;
		}
		catch (const std::exception& exception)
		{
			error = exception.what();
			return false;
		}
	}

	// ------------------------------------------------------------------------------------------
	// Scan
	// ------------------------------------------------------------------------------------------

	void StartScan()
	{
		g_scan_running = true;
		g_scan_level = 0;
		g_scan_actor = 0;
		g_scan_started = Clock::now();
		g_sightings.clear();
		g_rider_index.clear();
		g_mount_index.clear();
		g_pairs.clear();
		g_managers.clear();
		g_refs.clear();
		g_manager_refs.clear();
		g_positions_now.clear();
		g_group_linked.clear();
		g_group_any.clear();
	}

	void Look(APrimalDinoCharacter* dino)
	{
		const ClassKind kind = KindOf(dino->ClassField());
		if (!kind.rider && !kind.mount)
			return;
		if (dino->IsDead() || dino->TargetingTeamField() >= kFirstPlayerTeam)
			return;

		Sighting sighting;
		sighting.role = kind.rider ? Role::Rider : Role::Mount;
		sighting.mission = dino->OwnerMissionField() != nullptr;

		ANPCZoneManager* manager = dino->DirectLinkNPCZoneManagerField();
		if (manager != nullptr)
		{
			sighting.manager = Key(manager);
			if (g_managers.find(sighting.manager) == g_managers.end())
			{
				g_managers.emplace(sighting.manager, DescribeManager(manager));
				g_manager_refs.emplace(sighting.manager, GetWeakReference(manager));
			}
		}

		if (const int group = dino->PatrolGroupIDField(); group > 0)
		{
			++g_group_linked[{ sighting.manager, group }];
			++g_group_any[group];
		}

		const uint64_t self = Key(dino);
		const FVector location = dino->GetLocation();
		sighting.creature = self;
		sighting.x = location.X;
		sighting.y = location.Y;
		sighting.z = location.Z;
		g_positions_now[self] = location;
		const auto before = g_positions_before.find(self);
		if (before != g_positions_before.end())
		{
			const double dx = location.X - before->second.X;
			const double dy = location.Y - before->second.Y;
			const double dz = location.Z - before->second.Z;
			sighting.known = true;
			sighting.moved = std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		if (kind.rider)
		{
			g_rider_index[self] = g_sightings.size();
			g_refs.emplace(self, GetWeakReference(dino));
			if (APrimalDinoCharacter* mount = dino->MountedDinoField().Get())
				g_pairs.push_back({ self, Key(mount), LinkMountedDino, {} });
			if (USceneComponent* root = dino->RootComponentField().Get())
				if (USceneComponent* parent = root->AttachParentField().Get())
					if (AActor* owner = parent->OwnerPrivateField(); owner != nullptr && owner != dino)
						g_pairs.push_back({ self, Key(owner), LinkAttachParent,
							owner->ClassField() != nullptr ? owner->ClassField()->NameField().ToString().ToString() : std::string() });
		}
		else
		{
			g_mount_index[self] = g_sightings.size();
			g_refs.emplace(self, GetWeakReference(dino));
			auto& seats = dino->PassengerPerSeatField();
			for (int32 i = 0; i < seats.Num(); ++i)
				if (APrimalCharacter* passenger = seats[i].Get())
					g_pairs.push_back({ Key(passenger), self, LinkPassengerSeat, {} });
			if (APrimalCharacter* carried = dino->CarriedCharacterField().Get())
				g_pairs.push_back({ Key(carried), self, LinkCarried, {} });
		}
		g_sightings.push_back(sighting);
	}

	std::string DescribePlan(const CleanPlan& plan)
	{
		std::ostringstream out;
		out << plan.remove.size() << " creature(s): " << plan.riders_on_foot << " rider(s) on foot and " << plan.empty_mounts
			<< " empty mount(s) of patrol managers, " << plan.unlinked_riders << " rider(s) and " << plan.unlinked_mounts
			<< " empty mount(s) without a manager link";
		return out.str();
	}

	CleanPlan CurrentPlan()
	{
		return PlanClean(g_last_sightings, g_settings.remove_unlinked, g_settings.remove_unpaired);
	}

	// Queues the plan of the last finished scan. Returns how many creatures were queued.
	size_t QueuePlan()
	{
		const CleanPlan plan = CurrentPlan();
		std::unordered_map<uint64_t, uint64_t> manager_of;
		for (const Sighting& sighting : g_last_sightings)
			manager_of[sighting.creature] = sighting.manager;

		g_queue.clear();
		g_queue_next = 0;
		g_removed = 0;
		g_skipped = 0;
		for (const CleanEntry& entry : plan.remove)
		{
			const auto ref = g_last_refs.find(entry.creature);
			if (ref != g_last_refs.end())
				g_queue.push_back({ ref->second, manager_of[entry.creature], entry.role == Role::Mount });
		}
		if (!g_queue.empty())
			PluginLog()->info("Clean queued: {}", DescribePlan(plan));
		return g_queue.size();
	}

	bool IsDino(AActor* actor)
	{
		return actor != nullptr && actor->IsA(APrimalDinoCharacter::GetPrivateStaticClass());
	}

	// True when the rider sits on a creature (patrol riders are attached to their mount).
	bool IsOnMount(APrimalDinoCharacter* rider)
	{
		if (rider->MountedDinoField().Get() != nullptr)
			return true;
		USceneComponent* root = rider->RootComponentField().Get();
		USceneComponent* parent = root != nullptr ? root->AttachParentField().Get() : nullptr;
		AActor* owner = parent != nullptr ? parent->OwnerPrivateField() : nullptr;
		return owner != rider && IsDino(owner);
	}

	// True when another creature is attached anywhere under one of the mount's components.
	bool HasAttachedCreature(USceneComponent* component, APrimalDinoCharacter* mount, int depth)
	{
		if (component == nullptr || depth > 4)
			return false;
		auto& children = component->AttachChildrenField();
		for (int32 i = 0; i < children.Num(); ++i)
		{
			USceneComponent* child = children[i].Get();
			if (child == nullptr)
				continue;
			AActor* owner = child->OwnerPrivateField();
			if (owner != mount && IsDino(owner))
				return true;
			if (owner == mount && HasAttachedCreature(child, mount, depth + 1))
				return true;
		}
		return false;
	}

	bool IsRidden(APrimalDinoCharacter* mount)
	{
		if (mount->CarriedCharacterField().Get() != nullptr)
			return true;
		auto& seats = mount->PassengerPerSeatField();
		for (int32 i = 0; i < seats.Num(); ++i)
			if (seats[i].Get() != nullptr)
				return true;
		return HasAttachedCreature(mount->RootComponentField().Get(), mount, 0);
	}

	// Removes up to RemovalsPerTick queued creatures, each checked again: still there, alive, unowned, not
	// part of a mission, still linked to the manager it had at the scan, and for a rider not on a mount,
	// for a mount not ridden.
	void DrainQueue()
	{
		int budget = g_settings.removals_per_tick;
		while (budget > 0 && g_queue_next < g_queue.size())
		{
			QueuedCreature& item = g_queue[g_queue_next++];
			APrimalDinoCharacter* creature = item.creature.Get();
			if (creature == nullptr || creature->IsDead() || creature->TargetingTeamField() >= kFirstPlayerTeam ||
				creature->OwnerMissionField() != nullptr ||
				Key(creature->DirectLinkNPCZoneManagerField()) != item.manager ||
				(item.mount ? IsRidden(creature) : IsOnMount(creature)))
			{
				++g_skipped;
				continue;
			}
			creature->Destroy(false, true);
			++g_removed;
			--budget;
		}
		if (g_queue_next >= g_queue.size())
		{
			PluginLog()->info("Clean finished: removed {} creature(s), {} skipped (gone, ridden or relinked since the scan)",
				g_removed, g_skipped);
			g_queue.clear();
			g_queue_next = 0;
		}
	}

	void FinishScan()
	{
		g_scan_running = false;

		// Riders and mounts can be seen in any order, so pairs are matched at the end. A rider sitting on
		// something outside MountPathContains still counts as mounted; those classes are named in the log.
		int link_pairs[LinkCount] = {};
		std::map<std::string, int> other_mounts;
		for (const Pair& pair : g_pairs)
		{
			const auto rider = g_rider_index.find(pair.rider);
			if (rider == g_rider_index.end())
				continue;
			const auto mount = g_mount_index.find(pair.mount);
			if (mount != g_mount_index.end())
				g_sightings[mount->second].mounted = true;
			else if (pair.link == LinkAttachParent)
				++other_mounts[pair.mount_class.empty() ? std::string("unknown") : pair.mount_class];
			g_sightings[rider->second].mounted = true;
			++link_pairs[pair.link];
		}
		{
			std::ostringstream links;
			links << "Mount links found:";
			for (int i = 0; i < LinkCount; ++i)
				links << (i == 0 ? " " : ", ") << kLinkNames[i] << " " << link_pairs[i];
			if (!other_mounts.empty())
			{
				std::vector<std::pair<int, std::string>> sorted;
				for (const auto& [name, count] : other_mounts)
					sorted.emplace_back(count, name);
				std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
				links << "; riders attached to other classes:";
				for (size_t i = 0; i < sorted.size() && i < 8; ++i)
					links << (i == 0 ? " " : ", ") << sorted[i].second << " " << sorted[i].first;
			}
			g_last_links = links.str();
		}

		// Live creatures per saved group, now that every creature has been seen.
		for (auto& [key, manager] : g_managers)
		{
			for (SavedGroup& group : manager.saved)
			{
				const auto linked = g_group_linked.find({ key, group.id });
				group.live_linked = linked != g_group_linked.end() ? linked->second : 0;
				const auto any = g_group_any.find(group.id);
				group.live_any = any != g_group_any.end() ? any->second : 0;
			}
		}

		g_last_report = BuildReport(g_sightings, g_settings.still_distance);
		g_last_finished = Clock::now();
		g_last_seconds = std::chrono::duration<double>(g_last_finished - g_scan_started).count();
		g_have_report = true;
		g_last_managers = g_managers;
		g_last_sightings = g_sightings;

		// A negative linked count stops a patrol manager from sending out patrols; set it back to 0.
		if (g_settings.reset_negative_count)
		{
			int fixed = 0;
			for (auto& [key, ref] : g_manager_refs)  // TWeakObjectPtr::Get is not const
			{
				ANPCZoneManager* manager = ref.Get();
				if (manager == nullptr)
					continue;
				float& linked = *GetNativePointerField<float*>(manager, "ANPCZoneManager.DirectLinkDinoCount");
				if (linked >= 0.0f)
					continue;
				PluginLog()->info("Linked count of {} was {:.0f}; set to 0", manager->NameField().ToString().ToString(), linked);
				linked = 0.0f;
				++fixed;
			}
			if (fixed > 0)
				PluginLog()->info("Reset the negative linked count of {} patrol manager(s)", fixed);
		}

		// Saved groups with nothing left of them hold the manager's size slots for good (see OrphanGroups);
		// dissolve them so the manager sends out patrols again.
		if (g_settings.dissolve_orphan_groups)
		{
			int dissolved = 0;
			for (const auto& [key, info] : g_last_managers)
			{
				const std::vector<int> ids = OrphanGroups(info);
				if (ids.empty())
					continue;
				const auto ref = g_manager_refs.find(key);
				if (ref == g_manager_refs.end())
					continue;
				ANPCZoneManager* manager = ref->second.Get();
				if (manager == nullptr || !manager->IsValidLowLevel())
					continue;
				try
				{
					dissolved += DissolveGroups(manager, ids);
					ManagerInfo after;
					ReadGroups(manager, after);
					std::ostringstream text;
					for (size_t i = 0; i < ids.size() && i < 12; ++i)
						text << (i == 0 ? "#" : ", #") << ids[i];
					if (ids.size() > 12)
						text << ", ...";
					PluginLog()->info("Dissolved {} saved patrol group(s) of {} with nothing left of them ({}); groups now "
						"small {}/{}, medium {}/{}, large {}/{}, saved {} (was {})", ids.size(), info.name, text.str(),
						after.groups[0], after.max_groups[0], after.groups[1], after.max_groups[1], after.groups[2],
						after.max_groups[2], after.saved.size(), info.saved.size());
				}
				catch (const std::exception& error)
				{
					PluginLog()->warn("Dissolving saved patrol groups of {} failed: {}", info.name, error.what());
				}
			}
			if (dissolved > 0)
				PluginLog()->info("Dissolved {} saved patrol group(s) with nothing left of them", dissolved);
		}
		g_last_refs.swap(g_refs);
		g_refs.clear();
		g_last_manager_refs.swap(g_manager_refs);
		g_manager_refs.clear();

		g_positions_before.swap(g_positions_now);
		g_positions_now.clear();
		g_sightings.clear();
		g_rider_index.clear();
		g_mount_index.clear();
		g_pairs.clear();
		g_group_linked.clear();
		g_group_any.clear();

		PluginLog()->info("Patrol scan ({:.1f} s):\n{}", g_last_seconds, FormatReport(g_last_report, g_last_managers, 0));

		if (g_auto_scan)
		{
			g_auto_scan = false;
			const size_t queued = QueuePlan();
			if (queued > 0)
				PluginLog()->info("Auto clean: queued {} creature(s)", queued);
		}
	}

	void StepScan()
	{
		UWorld* world = World();
		if (world == nullptr)
			return;

		UClass* dino_class = APrimalDinoCharacter::GetPrivateStaticClass();
		int budget = g_settings.scan_actors_per_tick;
		auto& levels = world->LevelsField();

		while (budget > 0)
		{
			if (g_scan_level >= static_cast<size_t>(levels.Num()))
			{
				FinishScan();
				return;
			}

			ULevel* level = levels[static_cast<int32>(g_scan_level)].Get();
			if (level == nullptr)
			{
				++g_scan_level;
				g_scan_actor = 0;
				continue;
			}

			TArray<AActor*>& actors = *GetNativePointerField<TArray<AActor*>*>(level, kLevelActorsField);
			while (budget > 0 && g_scan_actor < actors.Num())
			{
				AActor* actor = actors[g_scan_actor++];
				--budget;
				if (actor == nullptr)
					continue;
				if (actor->IsA(dino_class))
					Look(static_cast<APrimalDinoCharacter*>(actor));
				else if (IsPatrolManager(actor))
				{
					// Recorded even with no creature linked, so managers with an empty roster show up.
					const uint64_t key = Key(actor);
					if (g_managers.find(key) == g_managers.end())
					{
						auto* manager = static_cast<ANPCZoneManager*>(actor);
						g_managers.emplace(key, DescribeManager(manager));
						g_manager_refs.emplace(key, GetWeakReference(manager));
					}
				}
			}
			if (g_scan_actor >= actors.Num())
			{
				++g_scan_level;
				g_scan_actor = 0;
			}
		}
	}

	void SafeTimer()
	{
		try
		{
			if (!g_settings_loaded)
				return;
			if (g_scan_running)
				StepScan();
			if (!g_queue.empty())
				DrainQueue();
			// Once after load: every load breaks the patrols again.
			if (!g_boot_clean_done && g_settings.clean_after_boot_minutes > 0 &&
				++g_seconds_since_load >= g_settings.clean_after_boot_minutes * 60 && !g_scan_running && g_queue.empty())
			{
				g_boot_clean_done = true;
				PluginLog()->info("Clean after boot: scanning");
				StartScan();
				g_auto_scan = true;
			}
			// Once after load, report only: lets DissolveOrphanGroups free the slots early.
			if (!g_boot_scan_done && g_settings.scan_after_boot_minutes > 0)
			{
				if (++g_seconds_since_load_scan >= g_settings.scan_after_boot_minutes * 60 && !g_scan_running && g_queue.empty())
				{
					g_boot_scan_done = true;
					PluginLog()->info("Scan after boot");
					StartScan();
				}
			}
			if (g_settings.auto_clean_minutes > 0 && --g_seconds_to_auto <= 0)
			{
				g_seconds_to_auto = g_settings.auto_clean_minutes * 60;
				if (!g_scan_running && g_queue.empty())
				{
					StartScan();
					g_auto_scan = true;
				}
			}
		}
		catch (const std::exception& exception)
		{
			PluginLog()->error("Scan failed: {}", exception.what());
			g_scan_running = false;
			g_auto_scan = false;
			g_queue.clear();
			g_queue_next = 0;
		}
	}

	// ------------------------------------------------------------------------------------------
	// Commands
	// ------------------------------------------------------------------------------------------

	std::string ReportCommandText(const std::string& argument)
	{
		std::istringstream words(argument);
		std::string mode;
		words >> mode;
		const bool verbose = ToLower(mode) == "verbose";
		if (!g_settings_loaded)
			return "PatrolRepair has no valid config; fix config.json and run " + std::string(kCmdReload);
		if (g_scan_running)
			return "A patrol scan is running; run " + std::string(kCmdReport) + " again in a few seconds.";

		std::ostringstream out;
		if (g_have_report)
		{
			const double age = std::chrono::duration<double>(Clock::now() - g_last_finished).count();
			out << "Patrol scan from " << static_cast<int>(age) << " s ago:\n"
				<< FormatReport(g_last_report, g_last_managers, verbose ? g_settings.report_managers : 0);
			if (verbose)
			{
				out << g_last_links << "\n";
				PluginLog()->info("Patrol scan details ({:.1f} s):\n{}{}\n{}\n{}\n{}\n{}", g_last_seconds,
					FormatReport(g_last_report, g_last_managers, (std::numeric_limits<int>::max)()), g_last_links,
					FormatSavedGroups(g_last_managers), FormatGroupObjects(g_last_managers), FormatSpawnChecks(g_last_managers),
					FormatPatrolPoints(g_last_managers));
			}
			else
				out << "For manager details, run " << kCmdReport << " verbose.\n";
		}
		else
		{
			out << "No patrol scan yet.\n";
		}
		out << "Started a new scan. Run " << kCmdReport << (verbose ? " verbose" : "") << " again in a few seconds.";
		StartScan();
		return out.str();
	}

	// Shows what a clean of the last scan would remove; "confirm" queues it, "cancel" stops a running one.
	std::string CleanCommandText(const std::string& arguments)
	{
		std::istringstream words(arguments);
		std::string word;
		bool confirm = false, cancel = false;
		while (words >> word)
		{
			if (ToLower(word) == "confirm")
				confirm = true;
			else if (ToLower(word) == "cancel")
				cancel = true;
		}

		if (cancel)
		{
			if (g_queue.empty())
				return "No clean is running.";
			const size_t left = g_queue.size() - g_queue_next;
			PluginLog()->info("Clean cancelled: removed {}, {} skipped, {} left", g_removed, g_skipped, left);
			g_queue.clear();
			g_queue_next = 0;
			return "Clean cancelled; " + std::to_string(g_removed) + " removed, " + std::to_string(left) + " left alone.";
		}
		if (!g_queue.empty())
			return "A clean is running: " + std::to_string(g_removed) + " removed, " + std::to_string(g_queue.size() - g_queue_next) +
				" to go. " + kCmdClean + " cancel stops it.";
		if (!g_have_report)
			return "No patrol scan yet; run " + std::string(kCmdReport) + " first.";

		const double age = std::chrono::duration<double>(Clock::now() - g_last_finished).count();
		const CleanPlan plan = CurrentPlan();
		std::ostringstream out;
		out << "From the scan " << static_cast<int>(age) << " s ago: " << DescribePlan(plan) << ".\n";
		if (plan.remove.empty())
			return out.str() + "Nothing to remove.";
		if (!confirm)
		{
			out << "Run " << kCmdClean << " confirm to remove them, " << g_settings.removals_per_tick << " per second.";
			return out.str();
		}
		if (age > kMaxPlanAgeSeconds)
			return out.str() + "That scan is too old; run " + kCmdReport + " twice, then " + kCmdClean + " confirm.";
		const size_t queued = QueuePlan();
		out << "Queued " << queued << "; about " << (queued + g_settings.removals_per_tick - 1) / g_settings.removals_per_tick
			<< " s at " << g_settings.removals_per_tick << " per second. The log says when it is done.";
		return out.str();
	}

	// Marks every patrol manager from the last scan for the game's own patrol reset: its saved reset
	// version is set to 0, below LoadForceResetPatrolsVersion. If the game honors the version, it resets
	// those managers' patrols on the next load. Takes effect only through saveworld and a restart.
	std::string ForceResetCommandText(const std::string& argument)
	{
		std::istringstream words(argument);
		std::string word;
		bool confirm = false;
		bool count_mode = false;  // "count": zero DirectLinkDinoCount now instead of marking the version
		while (words >> word)
		{
			if (ToLower(word) == "confirm")
				confirm = true;
			else if (ToLower(word) == "count")
				count_mode = true;
		}

		if (!g_have_report)
			return "No patrol scan yet; run " + std::string(kCmdReport) + " first.";
		const double age = std::chrono::duration<double>(Clock::now() - g_last_finished).count();
		if (!confirm)
		{
			std::ostringstream out;
			out << "From the scan " << static_cast<int>(age) << " s ago: " << g_last_manager_refs.size() << " patrol manager(s). ";
			if (count_mode)
				out << "Run " << kCmdForceReset << " count confirm to set their linked count (DirectLinkDinoCount) to 0 now.";
			else
				out << "Run " << kCmdForceReset << " confirm to set their saved patrol reset version to 0, then saveworld and restart.";
			return out.str();
		}

		int marked = 0, gone = 0;
		for (auto& [key, ref] : g_last_manager_refs)  // TWeakObjectPtr::Get is not const
		{
			ANPCZoneManager* manager = ref.Get();
			if (manager == nullptr)
			{
				++gone;
				continue;
			}
			if (count_mode)
			{
				float& linked = *GetNativePointerField<float*>(manager, "ANPCZoneManager.DirectLinkDinoCount");
				PluginLog()->info("ForceReset count: {} linked count {:.0f} -> 0", manager->NameField().ToString().ToString(), linked);
				linked = 0.0f;
				++marked;
				continue;
			}
			int& saved = *GetNativePointerField<int*>(manager, "ANPCZoneManager.SaveForceResetPatrolsVersion");
			const int expected = *GetNativePointerField<int*>(manager, "ANPCZoneManager.LoadForceResetPatrolsVersion");
			PluginLog()->info("ForceReset: {} reset version {}/{} -> 0/{}", manager->NameField().ToString().ToString(),
				saved, expected, expected);
			saved = 0;
			++marked;
		}
		PluginLog()->info("ForceReset{}: changed {} patrol manager(s), {} gone since the scan", count_mode ? " count" : "", marked, gone);
		std::ostringstream out;
		if (count_mode)
		{
			out << "Set the linked count to 0 on " << marked << " patrol manager(s) (" << gone << " gone since the scan). Run "
				<< kCmdReport << " verbose twice: it should show linked count 0.";
			return out.str();
		}
		out << "Marked " << marked << " patrol manager(s) for reset (" << gone << " gone since the scan). Run "
			<< kCmdReport << " verbose twice: it should show reset version 0/1. Then saveworld and restart.";
		return out.str();
	}

	std::string CommandArgument(const FString* command)
	{
		if (command == nullptr)
			return {};
		std::istringstream words(command->ToString());
		std::string word;
		words >> word; // the command itself
		std::string rest;
		std::getline(words, rest);
		return rest;
	}

	std::string ReloadCommandText()
	{
		std::string error;
		if (!ReloadSettings(error))
		{
			PluginLog()->error("Config reload failed: {}", error);
			return "PatrolRepair reload failed, old settings kept: " + error;
		}
		std::string text = "PatrolRepair reloaded";
		if (!g_settings.warnings.empty())
			text += "; " + std::to_string(g_settings.warnings.size()) + " warning(s) in the log";
		return text;
	}

	void SendAdmin(AShooterPlayerController* player, const std::string& text, bool ok = true)
	{
		std::istringstream lines(text);
		std::string line;
		while (std::getline(lines, line))
		{
			const std::wstring wide = AsaApi::Tools::Utf8Decode(line);
			AsaApi::GetApiUtils().SendServerMessage(player, ok ? FColorList::Green : FColorList::Red, L"{}", wide.c_str());
		}
	}

	AShooterPlayerController* AdminFrom(APlayerController* controller)
	{
		auto* player = static_cast<AShooterPlayerController*>(controller);
		if (player == nullptr || !player->bIsAdmin()())
			return nullptr;
		return player;
	}

	void ConsoleReport(APlayerController* controller, FString* command, bool)
	{
		if (AShooterPlayerController* admin = AdminFrom(controller))
			SendAdmin(admin, ReportCommandText(CommandArgument(command)));
	}

	void ConsoleClean(APlayerController* controller, FString* command, bool)
	{
		if (AShooterPlayerController* admin = AdminFrom(controller))
			SendAdmin(admin, CleanCommandText(CommandArgument(command)));
	}

	void ConsoleForceReset(APlayerController* controller, FString* command, bool)
	{
		if (AShooterPlayerController* admin = AdminFrom(controller))
			SendAdmin(admin, ForceResetCommandText(CommandArgument(command)));
	}

	void ConsoleReload(APlayerController* controller, FString*, bool)
	{
		if (AShooterPlayerController* admin = AdminFrom(controller))
		{
			const std::string text = ReloadCommandText();
			SendAdmin(admin, text, text.find("failed") == std::string::npos);
		}
	}

	void RconReply(RCONClientConnection* connection, RCONPacket* packet, const std::string& text)
	{
		FString reply(text);
		connection->SendMessageW(packet->Id, 0, &reply);
	}

	void RconReport(RCONClientConnection* connection, RCONPacket* packet, UWorld*)
	{
		RconReply(connection, packet, ReportCommandText(CommandArgument(&packet->Body)));
	}

	void RconClean(RCONClientConnection* connection, RCONPacket* packet, UWorld*)
	{
		RconReply(connection, packet, CleanCommandText(CommandArgument(&packet->Body)));
	}

	void RconForceReset(RCONClientConnection* connection, RCONPacket* packet, UWorld*)
	{
		RconReply(connection, packet, ForceResetCommandText(CommandArgument(&packet->Body)));
	}

	void RconReload(RCONClientConnection* connection, RCONPacket* packet, UWorld*)
	{
		RconReply(connection, packet, ReloadCommandText());
	}
} // namespace

// Called by AsaApi when the plugin is loaded.
extern "C" __declspec(dllexport) void Plugin_Init()
{
	Log::Get().Init(PROJECT_NAME);
	PluginLog()->info("PatrolRepair loaded (built {} {})", __DATE__, __TIME__);

	std::string error;
	if (!ReloadSettings(error))
		PluginLog()->error("Config not loaded: {}; fix config.json and run {}", error, kCmdReload);

	AsaApi::GetCommands().AddConsoleCommand(kCmdReport, &ConsoleReport);
	AsaApi::GetCommands().AddConsoleCommand(kCmdClean, &ConsoleClean);
	AsaApi::GetCommands().AddConsoleCommand(kCmdReload, &ConsoleReload);
	AsaApi::GetCommands().AddConsoleCommand(kCmdForceReset, &ConsoleForceReset);
	AsaApi::GetCommands().AddRconCommand(kCmdReport, &RconReport);
	AsaApi::GetCommands().AddRconCommand(kCmdClean, &RconClean);
	AsaApi::GetCommands().AddRconCommand(kCmdReload, &RconReload);
	AsaApi::GetCommands().AddRconCommand(kCmdForceReset, &RconForceReset);
	AsaApi::GetCommands().AddOnTimerCallback(kTimerId, &SafeTimer);

	g_spawn_hooked = AsaApi::GetHooks().SetHook(kSpawnPatrolGroupFunc, &Hook_ANPCZoneManager_SpawnPatrolGroup,
		&ANPCZoneManager_SpawnPatrolGroup_original);
	if (!g_spawn_hooked)
		PluginLog()->warn("Could not hook {}; SpawnPatrolGroup trace and spawn entries restore off", kSpawnPatrolGroupFunc);
	g_inner_hooked[0] = AsaApi::GetHooks().SetHook(kIsValidEntryFunc, &Hook_ANPCZoneManager_IsValidNPCEntry,
		&ANPCZoneManager_IsValidNPCEntry_original);
	g_inner_hooked[1] = AsaApi::GetHooks().SetHook(kSpawnFromEntriesFunc, &Hook_ANPCZoneManager_SpawnFromEntries,
		&ANPCZoneManager_SpawnFromEntries_original);
	g_inner_hooked[2] = AsaApi::GetHooks().SetHook(kSpawnNpcFunc, &Hook_ANPCZoneManager_SpawnNPC, &ANPCZoneManager_SpawnNPC_original);
	g_inner_hooked[3] = AsaApi::GetHooks().SetHook(kInjectLatentFunc, &Hook_AHibernationManager_InjectLatentSpawn,
		&AHibernationManager_InjectLatentSpawn_original);
	const char* const inner_names[4] = { kIsValidEntryFunc, kSpawnFromEntriesFunc, kSpawnNpcFunc, kInjectLatentFunc };
	for (int i = 0; i < 4; ++i)
		if (!g_inner_hooked[i])
			PluginLog()->warn("Could not hook {}; its count in the SpawnPatrolGroup trace stays 0", inner_names[i]);
}

// Called by AsaApi when the plugin is unloaded.
extern "C" __declspec(dllexport) void Plugin_Unload()
{
	if (g_spawn_hooked)
		AsaApi::GetHooks().DisableHook(kSpawnPatrolGroupFunc, &Hook_ANPCZoneManager_SpawnPatrolGroup);
	if (g_inner_hooked[0])
		AsaApi::GetHooks().DisableHook(kIsValidEntryFunc, &Hook_ANPCZoneManager_IsValidNPCEntry);
	if (g_inner_hooked[1])
		AsaApi::GetHooks().DisableHook(kSpawnFromEntriesFunc, &Hook_ANPCZoneManager_SpawnFromEntries);
	if (g_inner_hooked[2])
		AsaApi::GetHooks().DisableHook(kSpawnNpcFunc, &Hook_ANPCZoneManager_SpawnNPC);
	if (g_inner_hooked[3])
		AsaApi::GetHooks().DisableHook(kInjectLatentFunc, &Hook_AHibernationManager_InjectLatentSpawn);
	AsaApi::GetCommands().RemoveOnTimerCallback(kTimerId);
	AsaApi::GetCommands().RemoveConsoleCommand(kCmdReport);
	AsaApi::GetCommands().RemoveConsoleCommand(kCmdClean);
	AsaApi::GetCommands().RemoveConsoleCommand(kCmdReload);
	AsaApi::GetCommands().RemoveConsoleCommand(kCmdForceReset);
	AsaApi::GetCommands().RemoveRconCommand(kCmdReport);
	AsaApi::GetCommands().RemoveRconCommand(kCmdClean);
	AsaApi::GetCommands().RemoveRconCommand(kCmdReload);
	AsaApi::GetCommands().RemoveRconCommand(kCmdForceReset);
}
