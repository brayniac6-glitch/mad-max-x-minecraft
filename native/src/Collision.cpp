#include "Collision.h"

#include "Link.h"

// SkyCraft's CollisionField "Stage A" (MIT, chasmlol), for Mad Max: Mad Max's physics raycast (static
// world only) is cast straight down at every block corner around the player; each 8 x 8 block
// column of the grid becomes a heightfield, streamed to Minecraft as exact triangles (the player's
// smooth collider) and 1/8-block voxels (everything else, and Minecraft's server). Steep steps
// between corners become walls through Minecraft's own step-up rule. Game thread only.
namespace madcraft::Collision
{
	namespace
	{
		constexpr int   kRegion = 8;             // must match MadCollision.REGION_SIZE
		constexpr int   kSub = 2;                // grid corners per block (2: every half block; catches small rocks)
		constexpr int   kCells = kRegion * kSub;  // grid cells across a region
		constexpr int   kRadius = 3;             // column-regions around the player (7 x 7 = 56 x 56 blocks)
		constexpr float kAbove = 4.0f;           // rays start this far above the player's feet...
		constexpr float kBelow = 100.0f;         // ...and reach this far below (tall drops aren't misses)
		constexpr float kRescanShift = 3.0f;     // re-scan a column-region once the player is this much higher/lower
		constexpr double kBudgetMs = 1.5;        // raycasting per frame
		constexpr float kNoGround = -1.0e30f;

		struct Column
		{
			float heights[kCells + 1][kCells + 1];  // MC y at each grid corner (every 1/kSub block), kNoGround = miss
			float         scannedFrom{ 0 };           // MC y the rays started at
			int           next{ 0 };                  // corners done so far (scan in progress)
			bool          complete{ false };
			int           misses{ 0 };                // corners without ground in the last scan
			std::uint64_t doneMs{ 0 };
		};

		std::unordered_map<std::uint64_t, Column> columns;
		std::uint32_t                            epoch = 1;
		bool                                     needClear = true;
		std::atomic<bool>                        resetRequested{ false };
		std::uint64_t                            raysCast = 0, raysHit = 0;
		std::uint64_t                            lastReportMs = 0;

		std::uint64_t Key(int a_rx, int a_rz)
		{
			return (std::uint64_t(std::uint32_t(a_rx)) << 32) | std::uint32_t(a_rz);
		}

		// One corner: straight down from above the player, MC coordinates in and out. Under a roof,
		// an upper floor or an overhang (a hit more than a step above the player's feet) the ray goes
		// on from just below it, so the player stands on the floor they're actually on, not the roof
		// (SkyCraft's multi-hit columns). Only a surface with nothing found under it (a hill, a
		// wall) stays as the ground above the feet.
		float ProbeCorner(double a_x, double a_z, float a_fromY)
		{
			const float feetY = a_fromY - kAbove;
			const float bottom = a_fromY - kAbove - kBelow;
			float       start = a_fromY;
			float       first = kNoGround;
			for (int layer = 0; layer < 3; ++layer) {
				const Vec3 from = MadMax::FromMc(a_x, start, a_z);
				const Vec3 to = MadMax::FromMc(a_x, bottom, a_z);
				Vec3       hit{};
				++raysCast;
				if (!MadMax::RaycastStatic(from, to, hit)) {
					return first;  // nothing further down: the overhang's top is all there is
				}
				++raysHit;
				const float y = static_cast<float>(MadMax::ToMc(hit).y);
				if (y <= feetY + 1.0f) {
					return y;  // a floor at or below the feet: walkable
				}
				if (first == kNoGround) {
					first = y;
				}
				// Above the feet: a roof (open space under it) or solid rock? Look up from the feet's
				// height. Under a roof that ray meets its underside; inside a rock it starts inside the
				// shape, which Havok ignores, so it meets nothing - and the rock's top stays the ground
				// (a wall Minecraft can't step up), instead of the ray below finding the ground under it.
				{
					const Vec3 upFrom = MadMax::FromMc(a_x, feetY + 0.5f, a_z);
					const Vec3 upTo = MadMax::FromMc(a_x, y + 0.5f, a_z);
					Vec3       under{};
					++raysCast;
					if (!MadMax::RaycastStatic(upFrom, upTo, under)) {
						return first;  // solid from the feet up to its top: rock, cliff, wall
					}
				}
				start = y - 0.15f;  // a roof: look for the floor under it
			}
			return first;
		}

		// Corners whose ray found nothing (Mad Max hasn't streamed that bit of physics in yet, mostly)
		// take the lowest ground around them, so the player doesn't drop through a hole meanwhile; the
		// column is rescanned (its misses) and real ground replaces the fill. Corners with no ground
		// anywhere near stay holes.
		Column Filled(const Column& a_col)
		{
			Column out = a_col;
			if (a_col.misses == 0) {
				return out;
			}
			for (int pass = 0; pass < 4; ++pass) {
				Column next = out;
				bool   changed = false;
				for (int z = 0; z <= kCells; ++z) {
					for (int x = 0; x <= kCells; ++x) {
						if (out.heights[z][x] != kNoGround) {
							continue;
						}
						float lowest = 1e30f;
						int   found = 0;
						for (int dz = -1; dz <= 1; ++dz) {
							for (int dx = -1; dx <= 1; ++dx) {
								const int nz = z + dz, nx = x + dx;
								if ((dz || dx) && nz >= 0 && nz <= kCells && nx >= 0 && nx <= kCells && out.heights[nz][nx] != kNoGround) {
									lowest = std::min(lowest, out.heights[nz][nx]);
									++found;
								}
							}
						}
						if (found >= 2) {
							next.heights[z][x] = lowest;
							changed = true;
						}
					}
				}
				out = next;
				if (!changed) {
					break;
				}
			}
			return out;
		}

		void SendRegion(int a_rx, int a_ry, int a_rz, const Column& a_scanned)
		{
			const Column a_col = Filled(a_scanned);
			auto&       link = Link::Get();
			const int   x0 = a_rx * kRegion, y0 = a_ry * kRegion, z0 = a_rz * kRegion;
			const float yLo = float(y0) - 1.0f, yHi = float(y0 + kRegion) + 1.0f;

			// Triangles: two per grid cell, kept if they reach into this region's height (with a margin).
			std::vector<std::uint8_t> tris(sizeof(proto::ColRegion));
			constexpr float           kStep = 1.0f / kSub;
			for (int dz = 0; dz < kCells; ++dz) {
				for (int dx = 0; dx < kCells; ++dx) {
					const float h00 = a_col.heights[dz][dx], h10 = a_col.heights[dz][dx + 1];
					const float h01 = a_col.heights[dz + 1][dx], h11 = a_col.heights[dz + 1][dx + 1];
					if (h00 == kNoGround || h10 == kNoGround || h01 == kNoGround || h11 == kNoGround) {
						continue;
					}
					const float lo = std::min({ h00, h10, h01, h11 }), hi = std::max({ h00, h10, h01, h11 });
					if (hi < yLo || lo > yHi) {
						continue;
					}
					const float ax = float(x0) + dx * kStep, bx = ax + kStep, az = float(z0) + dz * kStep, bz = az + kStep;
					// Counter-clockwise seen from above (normals up), as in MadTri.
					const proto::ColTri t1{ { ax, h00, az, ax, h01, bz, bx, h11, bz }, 0 };
					const proto::ColTri t2{ { ax, h00, az, bx, h11, bz, bx, h10, az }, 0 };
					const auto*         p1 = reinterpret_cast<const std::uint8_t*>(&t1);
					const auto*         p2 = reinterpret_cast<const std::uint8_t*>(&t2);
					tris.insert(tris.end(), p1, p1 + sizeof(t1));
					tris.insert(tris.end(), p2, p2 + sizeof(t2));
				}
			}
			const auto triCount = static_cast<std::uint32_t>((tris.size() - sizeof(proto::ColRegion)) / sizeof(proto::ColTri));
			*reinterpret_cast<proto::ColRegion*>(tris.data()) = { x0, y0, z0, x0 + kRegion - 1, y0 + kRegion - 1, z0 + kRegion - 1, epoch, triCount };
			link.WriteCollision(proto::kColTris, tris.data(), static_cast<std::uint32_t>(tris.size()));

			// Voxels: solid from the region's bottom up to each block's LOWEST corner, rounded down to
			// 1/8 block (never above the triangles: Minecraft's server checks moves against these).
			std::vector<std::uint8_t> buf(sizeof(proto::ColRegion));
			std::uint32_t             count = 0;
			for (int dz = 0; dz < kRegion; ++dz) {
				for (int dx = 0; dx < kRegion; ++dx) {
					// The block's lowest grid corner (never above the triangles over it).
					float lowest = 1e30f;
					bool  missing = false;
					for (int sz = 0; sz <= kSub && !missing; ++sz) {
						for (int sx = 0; sx <= kSub; ++sx) {
							const float h = a_col.heights[dz * kSub + sz][dx * kSub + sx];
							if (h == kNoGround) {
								missing = true;
								break;
							}
							lowest = std::min(lowest, h);
						}
					}
					if (missing) {
						continue;
					}
					const int topEighths = static_cast<int>(std::floor(lowest * 8.0f));
					for (int by = y0; by < y0 + kRegion; ++by) {
						const int layers = std::clamp(topEighths - by * 8, 0, 8);
						if (layers == 0) {
							break;
						}
						proto::ColBlock block{};
						block.x = x0 + dx;
						block.y = by;
						block.z = z0 + dz;
						for (int l = 0; l < layers; ++l) {
							block.bits[l] = ~0ull;
						}
						const auto* p = reinterpret_cast<const std::uint8_t*>(&block);
						buf.insert(buf.end(), p, p + sizeof(block));
						++count;
					}
				}
			}
			*reinterpret_cast<proto::ColRegion*>(buf.data()) = { x0, y0, z0, x0 + kRegion - 1, y0 + kRegion - 1, z0 + kRegion - 1, epoch, count };
			link.WriteCollision(proto::kColRegion, buf.data(), static_cast<std::uint32_t>(buf.size()));
		}

		// A finished column: every vertical region its ground passes through, plus the ones around
		// the player's feet (known-empty, so Minecraft stops holding a teleported player).
		void SendColumn(int a_rx, int a_rz, const Column& a_col, double a_feetY)
		{
			float lo = 1e30f, hi = -1e30f;
			for (const auto& row : a_col.heights) {
				for (const float h : row) {
					if (h != kNoGround) {
						lo = std::min(lo, h);
						hi = std::max(hi, h);
					}
				}
			}
			const int feetRy = static_cast<int>(std::floor(a_feetY / kRegion));
			int       ry0 = feetRy - 2, ry1 = feetRy + 1;
			if (lo <= hi) {
				ry0 = std::min(ry0, static_cast<int>(std::floor((lo - 10.0f) / kRegion)));
				ry1 = std::max(ry1, static_cast<int>(std::floor((hi + 4.0f) / kRegion)));
			}
			ry0 = std::max(ry0, feetRy - 8);  // a cliff's full drop isn't needed
			for (int ry = ry0; ry <= ry1; ++ry) {
				SendRegion(a_rx, ry, a_rz, a_col);
			}
		}
	}

	float GroundAt(double a_x, double a_z)
	{
		const int rx = static_cast<int>(std::floor(a_x / kRegion)), rz = static_cast<int>(std::floor(a_z / kRegion));
		const auto it = columns.find(Key(rx, rz));
		if (it == columns.end() || !it->second.complete) {
			return kNoGround;
		}
		const double gx = (a_x - rx * kRegion) * kSub, gz = (a_z - rz * kRegion) * kSub;
		const int    cx = std::clamp(static_cast<int>(std::floor(gx)), 0, kCells - 1);
		const int    cz = std::clamp(static_cast<int>(std::floor(gz)), 0, kCells - 1);
		// The surface at this exact point (bilinear across the block's corners), like the triangles
		// Minecraft stands on: on a slope the highest corner can be blocks above the player.
		const auto& h = it->second.heights;
		const float c00 = h[cz][cx], c10 = h[cz][cx + 1], c01 = h[cz + 1][cx], c11 = h[cz + 1][cx + 1];
		if (c00 == kNoGround || c10 == kNoGround || c01 == kNoGround || c11 == kNoGround) {
			return kNoGround;
		}
		const float fx = static_cast<float>(gx - cx), fz = static_cast<float>(gz - cz);
		return (c00 * (1 - fx) + c10 * fx) * (1 - fz) + (c01 * (1 - fx) + c11 * fx) * fz;
	}

	bool Available()
	{
		return MadMax::RaycastAvailable();
	}

	void RequestReset()
	{
		resetRequested = true;
	}

	std::uint32_t Epoch()
	{
		return epoch;
	}

	void Update(const McVec& a_feet)
	{
		if (!Available()) {
			return;
		}
		auto& link = Link::Get();
		if (resetRequested.exchange(false)) {
			columns.clear();
			needClear = true;
		}
		if (needClear) {
			++epoch;
			if (!link.WriteCollision(proto::kColClear, &epoch, sizeof(epoch))) {
				return;  // ring full: try again next frame
			}
			needClear = false;
			logger::info("collision: cleared (epoch {})", epoch);
		}

		LARGE_INTEGER freq, start, now;
		::QueryPerformanceFrequency(&freq);
		::QueryPerformanceCounter(&start);
		const auto elapsedMs = [&] {
			::QueryPerformanceCounter(&now);
			return double(now.QuadPart - start.QuadPart) * 1000.0 / double(freq.QuadPart);
		};

		// Self-check, every 10 s: the ground straight under the player should be at their feet.
		static std::uint64_t lastCheckMs = 0;
		if (::GetTickCount64() - lastCheckMs > 10000) {
			lastCheckMs = ::GetTickCount64();
			const float ground = ProbeCorner(a_feet.x, a_feet.z, static_cast<float>(a_feet.y) + kAbove);
			if (ground == kNoGround) {
				logger::info("collision check: no ground under the player (feet y {:.2f})", a_feet.y);
			} else {
				logger::info("collision check: ground {:.2f} under feet {:.2f} (diff {:+.2f})", ground, a_feet.y, ground - a_feet.y);
			}
		}

		const int   prx = static_cast<int>(std::floor(a_feet.x / kRegion));
		const int   prz = static_cast<int>(std::floor(a_feet.z / kRegion));
		const float fromY = static_cast<float>(a_feet.y) + kAbove;

		// Nearest first: rings outward from the player's column.
		for (int ring = 0; ring <= kRadius; ++ring) {
			for (int dz = -ring; dz <= ring; ++dz) {
				for (int dx = -ring; dx <= ring; ++dx) {
					if (std::max(std::abs(dx), std::abs(dz)) != ring) {
						continue;
					}
					const int rx = prx + dx, rz = prz + dz;
					auto&     col = columns[Key(rx, rz)];
					// Done, unless the player has moved well up/down since, or it had misses: Mad Max
					// streams its physics in around the player, so ground scanned too early (loading,
					// just after a teleport) isn't there yet. Those retry every few seconds.
					const bool stale = col.misses > 0 && ::GetTickCount64() - col.doneMs > 3000;
					if (col.complete && std::abs(col.scannedFrom - fromY) < kRescanShift && !stale) {
						continue;
					}
					// At most every 2 s per column (a falling player would otherwise rescan everything
					// every frame).
					if (col.complete && ::GetTickCount64() - col.doneMs < 2000) {
						continue;
					}
					if (col.complete || col.next == 0) {
						col.complete = false;
						col.next = 0;
						col.misses = 0;
						col.scannedFrom = fromY;
					}
					constexpr int kCorners = (kCells + 1) * (kCells + 1);
					while (col.next < kCorners) {
						const int cz = col.next / (kCells + 1), cx = col.next % (kCells + 1);
						col.heights[cz][cx] = ProbeCorner(rx * kRegion + double(cx) / kSub, rz * kRegion + double(cz) / kSub, col.scannedFrom);
						col.misses += col.heights[cz][cx] == kNoGround ? 1 : 0;
						++col.next;
						if (elapsedMs() > kBudgetMs) {
							return;
						}
					}
					col.complete = true;
					col.doneMs = ::GetTickCount64();
					SendColumn(rx, rz, col, a_feet.y);
				}
			}
		}

		// Forget columns far behind (they're re-scanned when the player comes back).
		if (columns.size() > 400) {
			std::erase_if(columns, [&](const auto& kv) {
				const int rx = static_cast<int>(std::int32_t(kv.first >> 32)), rz = static_cast<int>(std::int32_t(kv.first & 0xffffffff));
				return std::max(std::abs(rx - prx), std::abs(rz - prz)) > kRadius + 4;
			});
		}

		const auto ms = ::GetTickCount64();
		if (ms - lastReportMs > 10000) {
			lastReportMs = ms;
			logger::info("collision: {} rays, {} hit ({:.0f}%), {} columns tracked", raysCast, raysHit, raysCast ? 100.0 * raysHit / raysCast : 0.0, columns.size());
		}
	}
}
