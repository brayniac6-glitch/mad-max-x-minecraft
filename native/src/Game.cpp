#include "Game.h"

#include "Collision.h"

// The per-frame bridge, modelled on SkyCraft's Game.cpp (MIT, chasmlol): Mad Max tells Minecraft
// where Max is; once Minecraft has arrived there, Minecraft's player drives Max.
//
// Phase 0 (this file): the link, teleport handshake, mouse look, puppeting Max's transform, and a
// flat collision floor at Max's ground height in place of Mad Max's real collision (which needs the
// Apex physics raycast from sheet/hooks.tsv, Phase 1).
namespace madcraft
{
	Runtime& State()
	{
		static Runtime runtime;
		return runtime;
	}

	namespace
	{
		constexpr float kGameTeleportThreshold = 12.0f;  // blocks; bigger jumps are Mad Max moving Max (fast travel, cutscene)
		constexpr float kLoadThreshold = 200.0f;         // blocks; bigger jumps are a load or fast travel
		constexpr int   kRegion = 8;                     // must match MadCollision.REGION_SIZE
		constexpr int   kFloorRadiusRegions = 3;         // 7 x 7 regions = 56 x 56 blocks around the player

		proto::McState mc{};
		bool           mcWasAlive = false;
		std::uint32_t  teleportSeq = [] {
			LARGE_INTEGER t;
			::QueryPerformanceCounter(&t);
			return static_cast<std::uint32_t>(t.QuadPart) | 1u;
		}();
		bool          teleportPending = true;
		std::uint32_t epoch = 1;
		bool          epochSent = false;
		McVec         lastSet{};
		bool          haveLastSet = false;
		std::int64_t  lastQpc = 0;
		std::atomic<DWORD> renderThread{ 0 };

		McVec              lastSafe{};
		bool               haveLastSafe = false;
		int                rescueFrames = 0;
		std::uint64_t      lastRescueMs = 0;
		std::atomic<float> groundUnderMax{ -1.0e30f };  // MC y of Mad Max's ground under Max (game thread)

		// Game-camera look (see Tick): the calibrated forward axis (row*2 + negated), -1 = unknown.
		const bool useGameCamera = IniBool("Camera", "bUseGameCamera", true);
		int        camAxis = -1;
		int        camCandidate = -1;
		int        camVotes = 0;

		// The pose Minecraft wants Max in, applied on the game thread (see GameThreadTick).
		struct Pose
		{
			bool  pending{ false };
			Vec3  feet{};
			float heading{ 0 };
		} pose;
		std::mutex poseLock;

		struct Diag
		{
			bool   started{ false };
			double startX{ 0 }, startZ{ 0 };
			float  seconds{ 0 };
			float  frames{ 0 };
			double drift{ 0 };
			double writeMiss{ 0 };
		} diag;

		// ---- Phase 0 floor -------------------------------------------------------------------
		struct Floor
		{
			bool   valid{ false };
			double y{ 0.0 };          // MC y of the walkable surface
			int    centreRx{ 0 }, centreRz{ 0 };
		} floor;

		void SendFloorRegion(int a_rx, int a_ry, int a_rz)
		{
			auto&     link = Link::Get();
			const int x0 = a_rx * kRegion, y0 = a_ry * kRegion, z0 = a_rz * kRegion;
			// The voxels must never stick up above the triangles: the player walks on the triangles
			// (client), but Minecraft's server checks moves against the voxels, and feet inside a
			// voxel make every step into a new block column "collide with something new" -> the
			// server silently sends the player back. So round the voxel top DOWN to 1/8 block.
			const int topEighths = static_cast<int>(std::floor(floor.y * 8.0));
			const int floorBlock = static_cast<int>(std::floor((topEighths - 1) / 8.0));
			const int layers = topEighths - floorBlock * 8;  // 1..8
			const bool hasFloor = floorBlock >= y0 && floorBlock < y0 + kRegion;

			// Triangles first (the player's smooth collider), then the voxels (everything else).
			struct TrisMsg
			{
				proto::ColRegion hdr;
				proto::ColTri    tri[2];
			} tris{};
			tris.hdr = { x0, y0, z0, x0 + kRegion - 1, y0 + kRegion - 1, z0 + kRegion - 1, epoch, hasFloor ? 2u : 0u };
			if (hasFloor) {
				const float fy = static_cast<float>(floor.y);
				const float ax = float(x0), bx = float(x0 + kRegion), az = float(z0), bz = float(z0 + kRegion);
				tris.tri[0] = { { ax, fy, az, ax, fy, bz, bx, fy, bz }, 0 };
				tris.tri[1] = { { ax, fy, az, bx, fy, bz, bx, fy, az }, 0 };
			}
			link.WriteCollision(proto::kColTris, &tris, sizeof(proto::ColRegion) + sizeof(proto::ColTri) * tris.hdr.count);

			std::vector<std::uint8_t> buf(sizeof(proto::ColRegion) + (hasFloor ? sizeof(proto::ColBlock) * kRegion * kRegion : 0));
			auto* region = reinterpret_cast<proto::ColRegion*>(buf.data());
			*region = { x0, y0, z0, x0 + kRegion - 1, y0 + kRegion - 1, z0 + kRegion - 1, epoch, hasFloor ? std::uint32_t(kRegion * kRegion) : 0u };
			if (hasFloor) {
				// Sub-voxel layers from the block's bottom up to (at most) the surface, 1/8 block each.
				auto* block = reinterpret_cast<proto::ColBlock*>(region + 1);
				for (int dz = 0; dz < kRegion; ++dz) {
					for (int dx = 0; dx < kRegion; ++dx, ++block) {
						*block = {};
						block->x = x0 + dx;
						block->y = floorBlock;
						block->z = z0 + dz;
						for (int l = 0; l < layers; ++l) {
							block->bits[l] = ~0ull;
						}
					}
				}
			}
			link.WriteCollision(proto::kColRegion, buf.data(), static_cast<std::uint32_t>(buf.size()));
		}

		// Lays (or re-centres) the floor under a_centre; a new height or a big move resends it.
		void UpdateFloor(const McVec& a_centre, double a_surfaceY, bool a_force)
		{
			const int rx = static_cast<int>(std::floor(a_centre.x / kRegion));
			const int rz = static_cast<int>(std::floor(a_centre.z / kRegion));
			const bool moved = !floor.valid || std::abs(rx - floor.centreRx) > 1 || std::abs(rz - floor.centreRz) > 1;
			const bool newHeight = !floor.valid || std::abs(a_surfaceY - floor.y) > 0.25;
			if (!a_force && !moved && !newHeight) {
				return;
			}
			// A rebuild is ~150 region messages and makes Minecraft drop and rebuild its collision:
			// at most once a second (cutscenes and loads move Max every frame).
			static std::uint64_t lastRebuildMs = 0;
			const std::uint64_t  now = ::GetTickCount64();
			if (!a_force && floor.valid && now - lastRebuildMs < 1000) {
				return;
			}
			lastRebuildMs = now;
			if (!epochSent || newHeight) {
				// A new height invalidates the old floor everywhere: new epoch, MC drops everything.
				if (epochSent) {
					++epoch;
				}
				Link::Get().WriteCollision(proto::kColClear, &epoch, sizeof(epoch));
				epochSent = true;
			}
			floor = { true, a_surfaceY, rx, rz };
			const int floorRy = static_cast<int>(std::floor(std::floor((std::floor(a_surfaceY * 8.0) - 1) / 8.0) / kRegion));
			for (int dz = -kFloorRadiusRegions; dz <= kFloorRadiusRegions; ++dz) {
				for (int dx = -kFloorRadiusRegions; dx <= kFloorRadiusRegions; ++dx) {
					// The floor's region and its neighbours above and below (empty, but known: Minecraft
					// holds a teleported player until the regions at, under and 8 blocks below its
					// feet have all arrived; see MadClient.holdUntilReady).
					for (int dy = -1; dy <= 1; ++dy) {
						SendFloorRegion(rx + dx, floorRy + dy, rz + dz);
					}
				}
			}
		}

		float FrameSeconds()
		{
			static const std::int64_t freq = [] { LARGE_INTEGER f; ::QueryPerformanceFrequency(&f); return f.QuadPart; }();
			LARGE_INTEGER             now;
			::QueryPerformanceCounter(&now);
			const float dt = lastQpc ? float(double(now.QuadPart - lastQpc) / double(freq)) : 0.0f;
			lastQpc = now.QuadPart;
			return std::clamp(dt, 0.0f, 0.25f);
		}
	}

	namespace Game
	{
		void GameThreadTick()
		{
			static bool logged = false;
			if (!logged) {
				logged = true;
				logger::info("threads: game input poll on {}, render (Present) on {}", ::GetCurrentThreadId(), renderThread.load());
			}
			if (::GetCurrentThreadId() == renderThread) {
				return;  // same thread after all: Tick() already ran; never write from Present
			}
			Pose p;
			{
				std::lock_guard g{ poseLock };
				p = pose;
			}
			if (p.pending && State().puppeting) {
				MadMax::SetPlayerPose(p.feet, p.heading);
			}
			// Mad Max's real ground around the player (raycasts must come from the game thread).
			Vec3 feet{};
			if (State().mcInWorld && Collision::Available() && MadMax::GetPlayerFeet(feet)) {
				Collision::Update(MadMax::ToMc(feet));
				// The ground under Max, for the fall rescue: from well above, so a Max already sunk
				// below it still finds it.
				Vec3 hit{};
				const Vec3 above{ feet.x, feet.y + 30.0f, feet.z }, below{ feet.x, feet.y - 60.0f, feet.z };
				groundUnderMax = MadMax::RaycastStatic(above, below, hit) ? static_cast<float>(MadMax::ToMc(hit).y) : -1.0e30f;
			}
		}

		void Tick()
		{
			renderThread = ::GetCurrentThreadId();
			auto& link = Link::Get();
			auto& st = State();
			if (!link.Valid()) {
				return;
			}
			const float dt = FrameSeconds();

			const bool mcAlive = link.McAlive();
			const bool haveMc = mcAlive && link.ReadMcState(mc);
			if (mcAlive != mcWasAlive) {
				logger::info("Minecraft {}", mcAlive ? "connected" : "gone");
				link.ResetOverlay();
				Collision::RequestReset();
				teleportPending = true;
				epochSent = false;
				floor.valid = false;
				if (!mcAlive) {
					st.puppeting = false;
					st.minecraftOwnsPlayer = false;
				}
			}
			mcWasAlive = mcAlive;
			st.mcInWorld = haveMc && (mc.flags & proto::kMcInWorld);
			const bool screenOpen = haveMc && (mc.flags & proto::kMcScreenOpen);
			if (screenOpen && !st.mcScreenOpen) {
				st.cursorX = st.viewportW / 2;
				st.cursorY = st.viewportH / 2;
			}
			st.mcScreenOpen = screenOpen;
			if (haveMc && mc.sensitivity > 0.0f) {
				st.sensitivity = mc.sensitivity;
			}

			Vec3       feet{};
			const bool inGame = MadMax::GetPlayerFeet(feet);
			const bool driving = inGame && MadMax::InVehicle();
			const auto feetMc = MadMax::ToMc(feet);
			st.bodyValid = inGame;
			st.bodyX = feetMc.x, st.bodyY = feetMc.y, st.bodyZ = feetMc.z;

			// Mad Max moved Max itself (fast travel, cutscene, loading a save): resync Minecraft.
			if (!inGame) {
				teleportPending = true;
				haveLastSet = false;
			} else if (haveLastSet) {
				const double gap = std::hypot(feetMc.x - lastSet.x, feetMc.y - lastSet.y, feetMc.z - lastSet.z);
				if (gap > kGameTeleportThreshold) {
					logger::info("Mad Max moved the player ({:.0f} blocks); resyncing Minecraft", gap);
					teleportPending = true;
					haveLastSet = false;
					if (gap > kLoadThreshold && !st.madMaxControls) {
						// A load or fast travel lands on a loading screen or cutscene: Mad Max's.
						st.madMaxControls = true;
						Input::ReleaseAll();
						logger::info("controls: Mad Max (load); F8 hands the player to Minecraft");
					}
				}
			}
			// Driving, or Mad Max controls chosen (F8): Mad Max has the player; Minecraft follows.
			static bool gameHadPlayer = false;
			const bool  gameHasPlayer = driving || st.madMaxControls;
			if (gameHadPlayer && !gameHasPlayer) {
				teleportPending = true;  // Minecraft picks up wherever Mad Max left Max
			}
			gameHadPlayer = gameHasPlayer;
			if (teleportPending && inGame) {
				++teleportSeq;
				teleportPending = false;
				float heading = 0.0f;
				if (MadMax::GetPlayerHeading(heading)) {
					st.yaw = MadMax::HeadingToMcYaw(heading);
				}
				st.pitch = 0.0f;
				st.lookInitialized = true;
				logger::info("teleport {} to {:.1f} {:.1f} {:.1f}", teleportSeq, feetMc.x, feetMc.y, feetMc.z);
			}

			// Look: Mad Max's own camera (the mouse turns it as usual) sets where Minecraft looks, so
			// W walks where you see. Which camera matrix row is "forward" isn't known up front: a
			// third-person camera looks at the player, so the row that points from the camera to
			// Max's head wins, once it has agreed for a moment (like SkyCraft's runtime axis check).
			bool camLook = false;
			if (useGameCamera && inGame) {
				float cm[16];
				if (MadMax::GetCameraMatrix(cm)) {
					const Vec3  camPos{ cm[12], cm[13], cm[14] };
					const float tx = feet.x - camPos.x, ty = feet.y + 1.6f - camPos.y, tz = feet.z - camPos.z;
					const float dist = std::sqrt(tx * tx + ty * ty + tz * tz);
					// Only in gameplay with Minecraft driving (the main menu's camera looks down at Max
					// from above, which once picked the camera's "down" axis), and never row 1: in a
					// Y-up camera that's the up/down axis, and walking along it flips with pitch.
					if (camAxis < 0 && st.puppeting && !driving && dist > 1.0f && dist < 15.0f) {
						int   best = -1;
						float bestDot = 0.0f;
						for (int c = 0; c < 6; ++c) {
							if (c / 2 == 1) {
								continue;
							}
							const int   r = c / 2;
							const float s = (c & 1) ? -1.0f : 1.0f;
							const float d = s * (cm[r * 4] * tx + cm[r * 4 + 1] * ty + cm[r * 4 + 2] * tz) / dist;
							if (d > bestDot) {
								bestDot = d, best = c;
							}
						}
						camVotes = (best == camCandidate && bestDot > 0.85f) ? camVotes + 1 : 0;
						camCandidate = best;
						if (camVotes >= 60) {
							camAxis = best;
							logger::info("camera: forward is {}row {} (agreed for 60 frames, alignment {:.2f})", (best & 1) ? "-" : "+", best / 2, bestDot);
						}
					}
					if (camAxis >= 0) {
						const int    r = camAxis / 2;
						const float  s = (camAxis & 1) ? -1.0f : 1.0f;
						const McVec  f = MadMax::ToMc({ s * cm[r * 4], s * cm[r * 4 + 1], s * cm[r * 4 + 2] });
						const double h = std::sqrt(f.x * f.x + f.z * f.z);
						if (h > 1e-4) {
							st.yaw = static_cast<float>(std::atan2(-f.x, f.z) * 57.29577951308232);
						}
						st.pitch = static_cast<float>(-std::atan2(f.y, h) * 57.29577951308232);
						st.lookInitialized = true;
						camLook = true;
					}
				}
			}
			st.cameraLook = camLook;

			// Mouse look (Minecraft's formula), integrated here so the view has no added latency.
			float dx = 0.0f, dy = 0.0f;
			Input::ConsumeLook(dx, dy);
			if (!camLook && !st.mcScreenOpen && !st.gameMenuOpen && !gameHasPlayer) {
				const float s = st.sensitivity * 0.6f + 0.2f;
				const float factor = s * s * s * 8.0f * 0.15f;
				st.yaw = std::fmod(st.yaw + dx * factor, 360.0f);
				st.pitch = std::clamp(st.pitch + dy * factor, -90.0f, 90.0f);
			}

			const bool arriving = haveMc && st.mcInWorld && inGame && mc.teleportAck != teleportSeq && !gameHasPlayer;
			const bool puppet = haveMc && st.mcInWorld && inGame && mc.teleportAck == teleportSeq && !gameHasPlayer;
			st.minecraftOwnsPlayer = puppet || arriving;
			if (puppet != st.puppeting) {
				logger::info("puppet {}", puppet ? "on (Minecraft drives Max)" : "off");
			}
			st.puppeting = puppet;
			st.mcCrosshair = puppet && mc.cameraMode == 0 && !st.mcScreenOpen && !st.gameMenuOpen;
			st.mcGuiScale = haveMc ? static_cast<int>(mc.guiScale) : 0;

			// Fall rescue: the collision field can have a hole (ground not streamed in yet). Minecraft
			// falling well below where it last stood puts Max back there and resyncs Minecraft.
			if (puppet && (mc.flags & proto::kMcOnGround)) {
				lastSafe = { mc.x, mc.y, mc.z };
				haveLastSafe = true;
			}
			// Only a fall THROUGH Mad Max's ground (a hole in the collision), never an ordinary drop
			// off a ledge: Minecraft well below the ground the game thread measured under Max. Once,
			// then a cooldown.
			const float ground = groundUnderMax.load();
			if (puppet && rescueFrames == 0 && ::GetTickCount64() - lastRescueMs > 2000 && ground > -1.0e29f &&
				!(mc.flags & proto::kMcOnGround) && !(mc.flags & proto::kMcFlying) && mc.y < ground - 2.0) {
				lastRescueMs = ::GetTickCount64();
				const McVec target = haveLastSafe ? lastSafe : McVec{ mc.x, double(ground) + 0.1, mc.z };
				logger::info("fall rescue: Minecraft at {:.1f} is under the ground ({:.1f}); back to {:.1f} {:.1f} {:.1f}", mc.y, ground, target.x, target.y, target.z);
				{
					std::lock_guard g{ poseLock };
					pose = { true, MadMax::FromMc(target.x, target.y + 0.1, target.z), pose.heading };
				}
				rescueFrames = 3;  // let the game thread put Max back, then teleport Minecraft to him
			}
			if (rescueFrames > 0) {
				if (--rescueFrames == 0) {
					teleportPending = true;
					haveLastSet = false;
				}
			} else if (puppet) {
				// What Max actually did since our last write: Mad Max's own movement fighting ours
				// shows up as Max lagging where we put him.
				// The write itself happens on Mad Max's game thread (GameThreadTick, from its input
				// poll): calling SetTransform from this render thread races Havok.
				Vec3 before{};
				const bool haveBefore = MadMax::GetPlayerFeet(before);
				{
					std::lock_guard g{ poseLock };
					pose = { true, MadMax::FromMc(mc.x, mc.y, mc.z), MadMax::McYawToHeading(mc.yaw) };
				}
				Vec3 after{};
				const bool haveAfter = MadMax::GetPlayerFeet(after);

				// Every 2 s: Minecraft's speed, how far Max strayed from our last write before this
				// one (the game moving him itself), whether the write took, and Mad Max's frame rate.
				diag.seconds += dt;
				diag.frames += 1;
				if (haveLastSet && haveBefore) {
					const auto b = MadMax::ToMc(before);
					diag.drift = std::max(diag.drift, std::hypot(b.x - lastSet.x, b.y - lastSet.y, b.z - lastSet.z));
				}
				if (haveAfter) {
					const auto a = MadMax::ToMc(after);
					diag.writeMiss = std::max(diag.writeMiss, std::hypot(a.x - mc.x, a.y - mc.y, a.z - mc.z));
				}
				if (!diag.started) {
					diag = { true, mc.x, mc.z };
				} else if (diag.seconds >= 2.0f) {
					const double moved = std::hypot(mc.x - diag.startX, mc.z - diag.startZ);
					// Only while something happens (moving, or keys held), so standing still stays quiet.
					const auto held = Input::DescribeHeld();
					if (moved > 0.01 || held.find("key") != std::string::npos || held.find("mouse") != std::string::npos) {
						const auto f = mc.flags;
						logger::info("diag: MC speed {:.2f} blocks/s [{}{}{}{}{}], held:{}, Max drift {:.2f}, write miss {:.3f}, {:.0f} fps",
							moved / diag.seconds, (f & proto::kMcOnGround) ? "ground" : "AIR", (f & proto::kMcSneaking) ? " SNEAK" : "",
							(f & proto::kMcSprinting) ? " sprint" : "", (f & proto::kMcSwimming) ? " SWIM" : "", (f & proto::kMcFlying) ? " FLY" : "",
							held, diag.drift, diag.writeMiss, diag.frames / diag.seconds);
					}
					diag = { true, mc.x, mc.z };
				}

				lastSet = { mc.x, mc.y, mc.z };
				haveLastSet = true;
			} else if (!puppet) {
				std::lock_guard g{ poseLock };
				pose.pending = false;
				if (inGame) {
					lastSet = feetMc;
					haveLastSet = true;
				}
			}

			// Tell Minecraft where Max is and where they're looking.
			proto::MadState out{};
			out.flags = (inGame ? proto::kSkyInGame : 0u) | (st.gameMenuOpen ? proto::kSkyMenuOpen : 0u) | (inGame ? 0u : proto::kSkyLoading);
			out.worldId = 1;  // one open world (Mad Max has no separate worldspaces/interiors)
			out.collisionEpoch = Collision::Available() ? Collision::Epoch() : epoch;
			out.posX = feetMc.x;
			out.posY = feetMc.y;
			out.posZ = feetMc.z;
			out.yaw = st.yaw;
			out.pitch = st.pitch;
			out.teleportSeq = teleportSeq;
			out.viewportW = static_cast<std::uint32_t>(st.viewportW.load());
			out.viewportH = static_cast<std::uint32_t>(st.viewportH.load());
			out.gameHour = 12.0f;  // TODO(hooks.tsv: TimeOfDay) until Mad Max's clock is found
			link.WriteGameState(out);

			// Phase 0 ground: while Minecraft drives, the floor stays at the height Max stood on when
			// it took over; otherwise it follows Max.
			if (haveMc && inGame && !Collision::Available()) {
				if (puppet) {
					UpdateFloor({ mc.x, mc.y, mc.z }, floor.valid ? floor.y : feetMc.y, false);
				} else {
					UpdateFloor(feetMc, feetMc.y, false);
				}
			}
		}
	}
}
