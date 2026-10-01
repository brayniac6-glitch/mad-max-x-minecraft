#include "Combat.h"

namespace madcraft::Combat
{
	namespace
	{
		bool  enabled = true;
		float range = 48.0f;         // blocks: characters further away aren't mirrored
		float npcMcHealth = 20.0f;   // a Mad Max NPC takes as many Minecraft hits as a mob with this much health
		float minDamagePct = 0.0f;

		std::uint64_t lastPublishMs = 0;
		std::uint64_t lastCountLogMs = 0;
		std::size_t   lastCount = static_cast<std::size_t>(-1);

		struct Known
		{
			std::uintptr_t object;
			float          maxHealth;
		};
		std::unordered_map<std::uint32_t, Known> known;  // actor id -> character, from the last publish
		std::vector<MadMax::Character>            scratch;

		std::uint64_t NowMs() { return ::GetTickCount64(); }

		// Stable per character while it lives (the object doesn't move).
		std::uint32_t IdOf(std::uintptr_t a_obj)
		{
			const auto v = static_cast<std::uint64_t>(a_obj);
			const auto id = static_cast<std::uint32_t>((v >> 4) ^ (v >> 36));
			return id ? id : 1;
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
					known[r.formId] = { c.object, c.maxHealth };
				}
			}
			Link::Get().WriteActors(records.data(), static_cast<std::uint32_t>(records.size()));
			if (known.size() != lastCount && NowMs() - lastCountLogMs > 5000) {
				lastCountLogMs = NowMs();
				lastCount = known.size();
				logger::info("combat: {} characters in the manager, {} alive within {} blocks", scratch.size(), known.size(), range);
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
			// Minecraft's damage as a share of a mob's health, applied to this character's health:
			// a wooden sword (4) takes five hits, a diamond sword (7) three, crits and Sharpness more.
			const float amount = std::max(mcDamage / npcMcHealth * it->second.maxHealth, minDamagePct * it->second.maxHealth);
			float       dealt = 0.0f;
			const bool  ok = MadMax::DamageCharacter(it->second.object, amount, dealt);
			float       hp[2]{};
			SafeRead(it->second.object + 0x180, hp, sizeof(hp));
			logger::info("combat: Minecraft hit {:08X} for {:.1f} (weapon {}, flags {:X}) -> {:.1f} Mad Max damage, game applied {:.1f}{}; health {:.1f}/{:.1f}",
				a_ev.formId, mcDamage, a_ev.weapon, a_ev.flags, amount, dealt, ok ? "" : " (call FAILED)", hp[1], hp[0]);
		}
	}

	void Init()
	{
		enabled = IniBool("Combat", "bEnabled", true);
		range = static_cast<float>(IniDouble("Combat", "fRange", 48.0));
		npcMcHealth = std::max(1.0f, static_cast<float>(IniDouble("Combat", "fNpcMinecraftHealth", 20.0)));
		minDamagePct = static_cast<float>(IniDouble("Combat", "fMinDamagePct", 0.0)) / 100.0f;
		logger::info("combat: {}; NPCs take hits like a {}-health Minecraft mob", enabled ? "on" : "off", npcMcHealth);
	}

	void Update(const Vec3& a_playerFeet)
	{
		auto& link = Link::Get();
		if (!enabled || !link.Valid()) {
			return;
		}
		// Hits first, against the characters Minecraft was shown.
		proto::McEvent ev{};
		while (link.PopEvent(ev)) {
			if (ev.type == proto::kEvHitActor) {
				Hit(ev);
			}
		}
		if (NowMs() - lastPublishMs >= 50) {
			lastPublishMs = NowMs();
			Publish(a_playerFeet);
		}
	}
}
