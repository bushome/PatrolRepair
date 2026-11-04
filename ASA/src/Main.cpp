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
//     runs a scan and clean once after load, Clean.AutoCleanMinutes on an interval. Nothing else changes
//     the world.
//
// Everything runs on the game thread (AsaApi's one-second timer callback and the commands).

#include "API/ARK/Ark.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
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
	std::string g_last_text;
	std::string g_last_full;
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

	// The manager's own bookkeeping. These fields are all in the game's symbol data.
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
			for (const std::string& warning : settings.warnings)
				Log::GetLog()->warn("Config: {}", warning);
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
			Log::GetLog()->info("Clean queued: {}", DescribePlan(plan));
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
			Log::GetLog()->info("Clean finished: removed {} creature(s), {} skipped (gone, ridden or relinked since the scan)",
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

		const Report report = BuildReport(g_sightings, g_settings.still_distance);
		g_last_text = FormatReport(report, g_managers, g_settings.report_managers);
		g_last_full = FormatReport(report, g_managers, 1 << 30);
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
				Log::GetLog()->info("Linked count of {} was {:.0f}; set to 0", manager->NameField().ToString().ToString(), linked);
				linked = 0.0f;
				++fixed;
			}
			if (fixed > 0)
				Log::GetLog()->info("Reset the negative linked count of {} patrol manager(s)", fixed);
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

		Log::GetLog()->info("Patrol scan ({:.1f} s):\n{}{}", g_last_seconds, g_last_full, g_last_links);

		if (g_auto_scan)
		{
			g_auto_scan = false;
			const size_t queued = QueuePlan();
			if (queued > 0)
				Log::GetLog()->info("Auto clean: queued {} creature(s)", queued);
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
				Log::GetLog()->info("Clean after boot: scanning");
				StartScan();
				g_auto_scan = true;
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
			Log::GetLog()->error("Scan failed: {}", exception.what());
			g_scan_running = false;
			g_auto_scan = false;
			g_queue.clear();
			g_queue_next = 0;
		}
	}

	// ------------------------------------------------------------------------------------------
	// Commands
	// ------------------------------------------------------------------------------------------

	std::string ReportCommandText()
	{
		if (!g_settings_loaded)
			return "PatrolRepair has no valid config; fix config.json and run " + std::string(kCmdReload);
		if (g_scan_running)
			return "A patrol scan is running; run " + std::string(kCmdReport) + " again in a few seconds.";

		std::ostringstream out;
		if (g_have_report)
		{
			const double age = std::chrono::duration<double>(Clock::now() - g_last_finished).count();
			out << "Patrol scan from " << static_cast<int>(age) << " s ago:\n" << g_last_text << g_last_links << "\n";
		}
		else
		{
			out << "No patrol scan yet.\n";
		}
		out << "Started a new scan; run " << kCmdReport << " again in a few seconds for it.";
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
			Log::GetLog()->info("Clean cancelled: removed {}, {} skipped, {} left", g_removed, g_skipped, left);
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
				Log::GetLog()->info("ForceReset count: {} linked count {:.0f} -> 0", manager->NameField().ToString().ToString(), linked);
				linked = 0.0f;
				++marked;
				continue;
			}
			int& saved = *GetNativePointerField<int*>(manager, "ANPCZoneManager.SaveForceResetPatrolsVersion");
			const int expected = *GetNativePointerField<int*>(manager, "ANPCZoneManager.LoadForceResetPatrolsVersion");
			Log::GetLog()->info("ForceReset: {} reset version {}/{} -> 0/{}", manager->NameField().ToString().ToString(),
				saved, expected, expected);
			saved = 0;
			++marked;
		}
		Log::GetLog()->info("ForceReset{}: changed {} patrol manager(s), {} gone since the scan", count_mode ? " count" : "", marked, gone);
		std::ostringstream out;
		if (count_mode)
		{
			out << "Set the linked count to 0 on " << marked << " patrol manager(s) (" << gone << " gone since the scan). Run "
				<< kCmdReport << " twice: it should show linked count 0.";
			return out.str();
		}
		out << "Marked " << marked << " patrol manager(s) for reset (" << gone << " gone since the scan). Run "
			<< kCmdReport << " twice: it should show reset version 0/1. Then saveworld and restart.";
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
			Log::GetLog()->error("Config reload failed: {}", error);
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

	void ConsoleReport(APlayerController* controller, FString*, bool)
	{
		if (AShooterPlayerController* admin = AdminFrom(controller))
			SendAdmin(admin, ReportCommandText());
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
		RconReply(connection, packet, ReportCommandText());
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

	std::string error;
	if (!ReloadSettings(error))
		Log::GetLog()->error("Config not loaded: {}; fix config.json and run {}", error, kCmdReload);

	AsaApi::GetCommands().AddConsoleCommand(kCmdReport, &ConsoleReport);
	AsaApi::GetCommands().AddConsoleCommand(kCmdClean, &ConsoleClean);
	AsaApi::GetCommands().AddConsoleCommand(kCmdReload, &ConsoleReload);
	AsaApi::GetCommands().AddConsoleCommand(kCmdForceReset, &ConsoleForceReset);
	AsaApi::GetCommands().AddRconCommand(kCmdReport, &RconReport);
	AsaApi::GetCommands().AddRconCommand(kCmdClean, &RconClean);
	AsaApi::GetCommands().AddRconCommand(kCmdReload, &RconReload);
	AsaApi::GetCommands().AddRconCommand(kCmdForceReset, &RconForceReset);
	AsaApi::GetCommands().AddOnTimerCallback(kTimerId, &SafeTimer);
}

// Called by AsaApi when the plugin is unloaded.
extern "C" __declspec(dllexport) void Plugin_Unload()
{
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
