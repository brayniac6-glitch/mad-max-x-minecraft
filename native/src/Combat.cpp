#include "Combat.h"

#include "PlayerHurt.h"

namespace madcraft::Combat
{
	namespace
	{
		bool  enabled = true;
		float range = 96.0f;         // blocks: characters further away aren't mirrored (War Criers sit on tall towers)
		float npcMcHealth = 20.0f;   // a Mad Max NPC takes as many Minecraft hits as a mob with this much health
		float minDamagePct = 0.0f;

		// Breakable objects (fuel tanks, explosives, ...): classes built on CDamageable, found by
		// scanning the heap for their vtables ([Combat] sBreakableVtables, H14).
		bool                        objectsOn = true;
		bool                        objectsArrowsOnly = true;
		float                       objectMcHealth = 6.0f;  // one full-strength arrow (6+) breaks one
		float                       objectRange = 64.0f;
		float                       objectWidth = 2.0f, objectHeight = 2.5f, objectDrop = 0.5f;
		std::uint64_t               objectScanMs = 12000;
		std::vector<std::uintptr_t> breakableVtables;
		std::vector<std::string>    breakableNames;
		std::uintptr_t              breakableDamageFn = 0;  // CDamageable's damage entry (slot 23), H10

		struct Breakable
		{
			std::uintptr_t object;
			std::uintptr_t vtable;
			std::size_t    which;
			Vec3           pos;
		};
		std::mutex             breakablesLock;
		std::vector<Breakable> breakables;  // from the last scan (they don't move)
		std::atomic<bool>      scannerStarted{ false };

		std::uint64_t lastPublishMs = 0;
		std::uint64_t lastCountLogMs = 0;
		std::size_t   lastCount = static_cast<std::size_t>(-1);

		struct Known
		{
			std::uintptr_t object;
			float          maxHealth;
			bool           isObject;
			std::uintptr_t vtable;  // objects: still this class when hit (else it's gone)
		};
		std::unordered_map<std::uint32_t, Known> known;  // actor id -> character / object, from the last publish
		std::vector<MadMax::Character>            scratch;

		std::uint64_t NowMs() { return ::GetTickCount64(); }

		// Stable per character while it lives (the object doesn't move).
		std::uint32_t IdOf(std::uintptr_t a_obj)
		{
			const auto v = static_cast<std::uint64_t>(a_obj);
			const auto id = static_cast<std::uint32_t>((v >> 4) ^ (v >> 36));
			return id ? id : 1;
		}

		// "MadMax.exe+121AA58 CDamageableObject, MadMax.exe+..." -> vtables and names.
		void ParseBreakables(const std::string& a_list)
		{
			breakableVtables.clear();
			breakableNames.clear();
			std::size_t start = 0;
			for (std::size_t i = 0; i <= a_list.size(); ++i) {
				if (i != a_list.size() && a_list[i] != ',') {
					continue;
				}
				std::string item = a_list.substr(start, i - start);
				start = i + 1;
				const auto plus = item.find('+');
				if (plus == std::string::npos) {
					continue;
				}
				std::string rest = item.substr(plus + 1);
				std::string name;
				if (const auto sp = rest.find_first_of(" \t"); sp != std::string::npos) {
					name = rest.substr(sp + 1);
					rest = rest.substr(0, sp);
				}
				name.erase(0, name.find_first_not_of(" \t"));
				try {
					breakableVtables.push_back(MadMax::ModuleBase() + std::stoull(rest, nullptr, 16));
					breakableNames.push_back(name.empty() ? std::string("object") : name);
				} catch (...) {
				}
			}
		}

		void ScannerLoop()
		{
			for (;;) {
				std::this_thread::sleep_for(std::chrono::milliseconds(objectScanMs));
				if (!Link::Get().McAlive() || !MadMax::PlayerAvailable()) {
					continue;
				}
				const auto start = NowMs();
				const auto hits = MadMax::FindObjectsByVtable(breakableVtables, 8192);
				std::vector<Breakable> found;
				std::vector<int>       perClass(breakableVtables.size(), 0);
				for (const auto& h : hits) {
					float hp = 0.0f, max = 0.0f;
					Vec3  pos{};
					if (MadMax::ReadHealth(h.object, hp, max) && MadMax::ReadObjectPosition(h.object, pos)) {
						found.push_back({ h.object, breakableVtables[h.which], h.which, pos });
						++perClass[h.which];
					}
				}
				std::string counts;
				for (std::size_t k = 0; k < perClass.size(); ++k) {
					counts += std::format(" {} {}", breakableNames[k], perClass[k]);
				}
				{
					std::lock_guard g{ breakablesLock };
					breakables = std::move(found);
				}
				static int logged = 0;
				if (logged++ < 3 || logged % 20 == 0) {
					logger::info("combat: breakable objects (heap scan, {} ms):{}", NowMs() - start, counts);
				}
			}
		}

		void Publish(const Vec3& a_player)
		{
			if (!MadMax::ListCharacters(scratch)) {
				return;
			}
			const auto me = MadMax::ToMc(a_player);
			std::vector<proto::ActorRecord> records;
			records.reserve(std::min<std::size_t>(scratch.size(), proto::kMaxActors));
			known.clear();
			for (const auto& c : scratch) {
				if (records.size() >= proto::kMaxActors) {
					break;
				}
				const auto p = MadMax::ToMc(c.feet);
				const double dx = p.x - me.x, dy = p.y - me.y, dz = p.z - me.z;
				if (dx * dx + dz * dz > static_cast<double>(range) * range || std::fabs(dy) > range) {
					continue;
				}
				// Max himself is held apart by the manager; skip anything standing exactly on him anyway.
				if (dx * dx + dz * dz < 0.01 && std::fabs(dy) < 0.5) {
					continue;
				}
				proto::ActorRecord r{};
				r.formId = IdOf(c.object);
				r.flags = proto::kActorHostile | (c.dead ? proto::kActorDead : 0u);
				r.x = static_cast<float>(p.x);
				r.y = static_cast<float>(p.y);
				r.z = static_cast<float>(p.z);
				r.yaw = MadMax::HeadingToMcYaw(c.heading);
				r.width = 0.7f;
				r.height = 1.85f;
				r.healthFrac = std::clamp(c.health / c.maxHealth, 0.0f, 1.0f);
				records.push_back(r);
				if (!c.dead) {
					known[r.formId] = { c.object, c.maxHealth, false, 0 };
				}
			}
			// Breakable objects near the player that still stand.
			std::size_t objects = 0;
			if (objectsOn) {
				std::lock_guard g{ breakablesLock };
				for (const auto& b : breakables) {
					if (records.size() >= proto::kMaxActors) {
						break;
					}
					const auto   p = MadMax::ToMc(b.pos);
					const double dx = p.x - me.x, dy = p.y - me.y, dz = p.z - me.z;
					if (dx * dx + dz * dz > static_cast<double>(objectRange) * objectRange || std::fabs(dy) > objectRange) {
						continue;
					}
					std::uintptr_t vt = 0;
					float          hp = 0.0f, max = 0.0f;
					if (!SafeRead(b.object, &vt, sizeof(vt)) || vt != b.vtable || !MadMax::ReadHealth(b.object, hp, max) || hp <= 0.0f) {
						continue;  // broken already, or gone
					}
					proto::ActorRecord r{};
					r.formId = IdOf(b.object);
					r.flags = proto::kActorObject;
					r.x = static_cast<float>(p.x);
					r.y = static_cast<float>(p.y) - objectDrop;
					r.z = static_cast<float>(p.z);
					r.width = objectWidth;
					r.height = objectHeight;
					r.healthFrac = std::clamp(hp / max, 0.0f, 1.0f);
					// No name: Minecraft would float it over the target.
					records.push_back(r);
					known[r.formId] = { b.object, max, true, b.vtable };
					++objects;
				}
			}
			Link::Get().WriteActors(records.data(), static_cast<std::uint32_t>(records.size()));
			if (known.size() != lastCount && NowMs() - lastCountLogMs > 5000) {
				lastCountLogMs = NowMs();
				lastCount = known.size();
				logger::info("combat: {} characters in the manager, {} alive within {} blocks; {} breakable objects within {}", scratch.size(),
					known.size() - objects, range, objects, objectRange);
			}
		}

		void Hit(const proto::McEvent& a_ev)
		{
			const auto it = known.find(a_ev.formId);
			if (it == known.end()) {
				logger::info("combat: hit on {:08X} ignored (no longer nearby)", a_ev.formId);
				return;
			}
			const float mcDamage = a_ev.a;
			if (!(mcDamage > 0.0f)) {
				return;
			}
			const auto& k = it->second;
			const bool  arrow = (a_ev.flags & proto::kHitProjectile) != 0;
			if (k.isObject) {
				std::uintptr_t vt = 0;
				if (!SafeRead(k.object, &vt, sizeof(vt)) || vt != k.vtable) {
					return;  // gone since
				}
				if (objectsArrowsOnly && !arrow) {
					logger::info("combat: {:08X} is an object; only arrows break it", a_ev.formId);
					return;
				}
			}
			// Minecraft's damage as a share of a mob's health, applied to this target's health: a wooden
			// sword (4) takes five hits on an NPC, a diamond sword (7) three; one good arrow breaks an object.
			const float mobHealth = k.isObject ? objectMcHealth : npcMcHealth;
			const float amount = std::max(mcDamage / mobHealth * k.maxHealth, minDamagePct * k.maxHealth);
			float       before = 0.0f, max = 0.0f;
			MadMax::ReadHealth(k.object, before, max);
			float dealt = 0.0f;
			bool  ok = false;
			// Objects only through CDamageable's own damage entry ([Combat] sBreakableDamageFn): other
			// classes put something else in that vtable slot. Anything else gets the health setter.
			std::uintptr_t fn = 0, vt = 0;
			const bool     standard = !k.isObject ||
			                      (breakableDamageFn && SafeRead(k.object, &vt, sizeof(vt)) && SafeRead(vt + 23 * sizeof(void*), &fn, sizeof(fn)) && fn == breakableDamageFn);
			if (standard) {
				ok = MadMax::DamageCharacter(k.object, amount, dealt);
			}
			// Turned down (only certain weapons count against it, e.g. War Criers): the game's own health
			// setter instead, so its death or destruction still plays (invulnerable targets stay put).
			bool forced = false;
			if (!(dealt > 0.0f) && before > 0.0f) {
				forced = MadMax::SetHealth(k.object, std::max(0.0f, before - amount));
			}
			float hp = 0.0f;
			MadMax::ReadHealth(k.object, hp, max);
			logger::info("combat: Minecraft {} {:08X}{} for {:.1f} (weapon {}, flags {:X}) -> {:.1f} Mad Max damage, game applied {:.1f}{}{}; health {:.1f} -> {:.1f}/{:.1f}",
				arrow ? "shot" : "hit", a_ev.formId, k.isObject ? " (object)" : "", mcDamage, a_ev.weapon, a_ev.flags, amount, dealt, ok ? "" : " (call FAILED)",
				forced ? ", set directly" : "", before, hp, max);
		}
	}

	void Init()
	{
		enabled = IniBool("Combat", "bEnabled", true);
		range = static_cast<float>(IniDouble("Combat", "fRange", 96.0));
		npcMcHealth = std::max(1.0f, static_cast<float>(IniDouble("Combat", "fNpcMinecraftHealth", 20.0)));
		minDamagePct = static_cast<float>(IniDouble("Combat", "fMinDamagePct", 0.0)) / 100.0f;
		objectsOn = IniBool("Combat", "bBreakableObjects", true);
		objectsArrowsOnly = IniBool("Combat", "bObjectsArrowsOnly", true);
		objectMcHealth = std::max(0.5f, static_cast<float>(IniDouble("Combat", "fObjectMinecraftHealth", 6.0)));
		objectRange = static_cast<float>(IniDouble("Combat", "fObjectRange", 64.0));
		objectWidth = static_cast<float>(IniDouble("Combat", "fObjectWidth", 2.0));
		objectHeight = static_cast<float>(IniDouble("Combat", "fObjectHeight", 2.5));
		objectDrop = static_cast<float>(IniDouble("Combat", "fObjectDrop", 0.5));
		objectScanMs = static_cast<std::uint64_t>(std::max(2.0, IniDouble("Combat", "fObjectScanSeconds", 12.0)) * 1000.0);
		ParseBreakables(IniString("Combat", "sBreakableVtables", ""));
		if (const auto fnText = IniString("Combat", "sBreakableDamageFn", ""); fnText.find('+') != std::string::npos) {
			try {
				breakableDamageFn = MadMax::ModuleBase() + std::stoull(fnText.substr(fnText.find('+') + 1), nullptr, 16);
			} catch (...) {
			}
		}
		logger::info("combat: {}; NPCs take hits like a {}-health Minecraft mob; {} breakable object classes ({})", enabled ? "on" : "off", npcMcHealth,
			breakableVtables.size(), objectsArrowsOnly ? "arrows only" : "any hit");
		if (enabled && objectsOn && !breakableVtables.empty() && !scannerStarted.exchange(true)) {
			std::thread(ScannerLoop).detach();
		}
	}

	std::uint32_t ActorId(std::uintptr_t a_object)
	{
		return IdOf(a_object);
	}

	void Update(const Vec3& a_playerFeet)
	{
		auto& link = Link::Get();
		if (!enabled || !link.Valid()) {
			return;
		}
		// Hits first, against the characters and objects Minecraft was shown.
		proto::McEvent ev{};
		while (link.PopEvent(ev)) {
			if (ev.type == proto::kEvHitActor) {
				Hit(ev);
			} else if (ev.type == proto::kEvPlayerDied) {
				PlayerHurt::KillMax();
			}
		}
		if (NowMs() - lastPublishMs >= 50) {
			lastPublishMs = NowMs();
			Publish(a_playerFeet);
		}
	}
}
