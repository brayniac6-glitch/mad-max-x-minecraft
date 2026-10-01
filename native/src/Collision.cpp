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
		constexpr int   kRadius = 3;             // column-regions around the player (7 x 7 = 56 x 56 blocks)
		constexpr float kAbove = 4.0f;           // rays start this far above the player's feet...
		constexpr float kBelow = 40.0f;          // ...and reach this far below
		constexpr float kRescanShift = 3.0f;     // re-scan a column-region once the player is this much higher/lower
		constexpr double kBudgetMs = 1.5;        // raycasting per frame
		constexpr float kNoGround = -1.0e30f;

		struct Column
		{
			float heights[kRegion + 1][kRegion + 1];  // MC y at each corner, kNoGround = miss
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
				start = y - 0.15f;  // look under that surface
			}
			return first;
		}

		void SendRegion(int a_rx, int a_ry, int a_rz, const Column& a_col)
		{
			auto&       link = Link::Get();
			const int   x0 = a_rx * kRegion, y0 = a_ry * kRegion, z0 = a_rz * kRegion;
			const float yLo = float(y0) - 1.0f, yHi = float(y0 + kRegion) + 1.0f;

			// Triangles: two per block, kept if they reach into this region's height (with a margin).
			std::vector<std::uint8_t> tris(sizeof(proto::ColRegion));
			for (int dz = 0; dz < kRegion; ++dz) {
				for (int dx = 0; dx < kRegion; ++dx) {
					const float h00 = a_col.heights[dz][dx], h10 = a_col.heights[dz][dx + 1];
					const float h01 = a_col.heights[dz + 1][dx], h11 = a_col.heights[dz + 1][dx + 1];
					if (h00 == kNoGround || h10 == kNoGround || h01 == kNoGround || h11 == kNoGround) {
						continue;
					}
					const float lo = std::min({ h00, h10, h01, h11 }), hi = std::max({ h00, h10, h01, h11 });
					if (hi < yLo || lo > yHi) {
						continue;
					}
					const float ax = float(x0 + dx), bx = ax + 1.0f, az = float(z0 + dz), bz = az + 1.0f;
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
					const float h00 = a_col.heights[dz][dx], h10 = a_col.heights[dz][dx + 1];
					const float h01 = a_col.heights[dz + 1][dx], h11 = a_col.heights[dz + 1][dx + 1];
					if (h00 == kNoGround || h10 == kNoGround || h01 == kNoGround || h11 == kNoGround) {
						continue;
					}
					const int topEighths = static_cast<int>(std::floor(std::min({ h00, h10, h01, h11 }) * 8.0f));
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
		const int   cx = std::clamp(static_cast<int>(std::floor(a_x - rx * kRegion)), 0, kRegion - 1);
		const int   cz = std::clamp(static_cast<int>(std::floor(a_z - rz * kRegion)), 0, kRegion - 1);
		const auto& h = it->second.heights;
		float       best = kNoGround;
		for (const float v : { h[cz][cx], h[cz][cx + 1], h[cz + 1][cx], h[cz + 1][cx + 1] }) {
			best = std::max(best, v);
		}
		return best;
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
					constexpr int kCorners = (kRegion + 1) * (kRegion + 1);
					while (col.next < kCorners) {
						const int cz = col.next / (kRegion + 1), cx = col.next % (kRegion + 1);
						col.heights[cz][cx] = ProbeCorner(double(rx * kRegion + cx), double(rz * kRegion + cz), col.scannedFrom);
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
