#pragma once

#include "madcraft_protocol.h"

namespace madcraft
{
	// Owner of the shared-memory mapping (Mad Max creates it; Minecraft opens it).
	// Ported from SkyCraft's Link (MIT, chasmlol); the byte layout is shared with the Fabric mod.
	class Link
	{
	public:
		static Link& Get();

		bool Create();
		[[nodiscard]] bool Valid() const { return base_ != nullptr; }

		// True if Minecraft has touched its heartbeat recently.
		[[nodiscard]] bool          McAlive() const;
		void                        Heartbeat();
		[[nodiscard]] std::uint32_t McPid() const;

		// Seqlock write of Mad Max -> MC state. Called once per frame (MC paces on seq).
		void WriteGameState(const proto::MadState& a_state);
		void WriteWaterGrid(const proto::WaterGrid& a_grid);
		// Seqlock read of MC -> Mad Max state. False if no consistent snapshot was obtained.
		bool ReadMcState(proto::McState& a_out) const;

		// Input ring (producer). Drops the event if MC has fallen a full ring behind.
		void PushInput(proto::InputType a_type, std::uint16_t a_code, std::int32_t a_a = 0, std::int32_t a_b = 0, std::int32_t a_c = 0);

		// Collision ring (producer, one thread only). False if the ring is full.
		bool WriteCollision(proto::ColType a_type, const void* a_payload, std::uint32_t a_bytes);

		void WriteActors(const proto::ActorRecord* a_records, std::uint32_t a_count);
		bool PopEvent(proto::McEvent& a_out);
		bool ReadWorldEntities(proto::WorldEntities& a_out) const;
		void DrainRender(const std::function<void(std::uint32_t, const std::uint8_t*, std::uint32_t)>& a_fn, std::uint64_t a_maxBytes);

		// Overlay triple buffer (consumer).
		bool                                       AcquireOverlayFrame();
		void                                       ResetOverlay();
		[[nodiscard]] const std::uint8_t*          FrontPixels() const;
		[[nodiscard]] const proto::OverlaySlotHdr* FrontHeader() const;

	private:
		template <class T>
		T* At(std::uint64_t a_off) const { return reinterpret_cast<T*>(base_ + a_off); }

		HANDLE        mapping_{ nullptr };
		std::uint8_t* base_{ nullptr };
		std::uint32_t overlayFront_{ 2 };
	};
}
