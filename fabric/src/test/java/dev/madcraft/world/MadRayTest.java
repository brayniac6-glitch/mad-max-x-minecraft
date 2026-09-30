package dev.madcraft.world;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;

import java.util.ArrayList;
import java.util.List;
import org.junit.jupiter.api.Test;

class MadRayTest {
	private static MadTri tri(boolean stairHelper, double... v) {
		float[] f = new float[9];
		for (int i = 0; i < 9; i++) {
			f[i] = (float) v[i];
		}
		return new MadTri(f, 0, stairHelper);
	}

	/** Flat ground at height y covering [-10, 10] in x and z. */
	private static List<MadTri> ground(double y) {
		List<MadTri> out = new ArrayList<>();
		out.add(tri(false, -10, y, -10, 10, y, -10, 10, y, 10));
		out.add(tri(false, -10, y, -10, 10, y, 10, -10, y, 10));
		return out;
	}

	@Test
	void hitsGroundFromAbove() {
		MadRay.Hit hit = MadRay.cast(ground(10.3), 0.5, 12.0, 0.5, 0.5, 8.0, 0.5);
		assertNotNull(hit);
		assertEquals(10.3, hit.y(), 1e-5);
		assertEquals(1.0, hit.ny(), 1e-9); // normal faces the ray origin
	}

	@Test
	void missesWhenSegmentStopsShort() {
		assertNull(MadRay.cast(ground(10.3), 0.5, 12.0, 0.5, 0.5, 11.0, 0.5));
	}

	@Test
	void normalFacesRayFromBelow() {
		MadRay.Hit hit = MadRay.cast(ground(10.3), 0.5, 8.0, 0.5, 0.5, 12.0, 0.5);
		assertNotNull(hit);
		assertEquals(-1.0, hit.ny(), 1e-9);
	}

	@Test
	void picksNearestOfSeveralSurfaces() {
		List<MadTri> tris = ground(10.0);
		tris.addAll(ground(5.0));
		MadRay.Hit hit = MadRay.cast(tris, 0.5, 12.0, 0.5, 0.5, 0.0, 0.5);
		assertEquals(10.0, hit.y(), 1e-5);
	}

	@Test
	void ignoresInvisibleStairRamps() {
		List<MadTri> tris = new ArrayList<>();
		tris.add(tri(true, -10, 11, -10, 10, 11, -10, 10, 11, 10));
		tris.add(tri(true, -10, 11, -10, 10, 11, 10, -10, 11, 10));
		tris.addAll(ground(10.0));
		assertEquals(10.0, MadRay.cast(tris, 0.5, 12.0, 0.5, 0.5, 8.0, 0.5).y(), 1e-5);
	}

	@Test
	void placedBlockSitsOnGroundSlightlySunk() {
		// Ground at 10.3: the block goes in cell y=10 (sunk 0.3), not floating in cell 11.
		MadRay.Hit low = MadRay.cast(ground(10.3), 0.5, 12.0, 0.5, 0.5, 8.0, 0.5);
		assertArrayEquals(new int[] { 0, 10, 0 }, MadRay.placementCell(low));
		// Ground at 10.8: the block goes on top (cell 11), floating at most 0.2.
		MadRay.Hit high = MadRay.cast(ground(10.8), 0.5, 12.0, 0.5, 0.5, 8.0, 0.5);
		assertArrayEquals(new int[] { 0, 11, 0 }, MadRay.placementCell(high));
	}

	@Test
	void projectileSticksInTheSurfaceCell() {
		MadRay.Hit hit = MadRay.cast(ground(10.0), 0.5, 12.0, 0.5, 0.5, 8.0, 0.5);
		assertArrayEquals(new int[] { 0, 9, 0 }, MadRay.surfaceCell(hit));
	}

	@Test
	void placementHitStaysWithinServerReachCheck() {
		// The server rejects a use-on-block whose hit point is a block or more from the cell centre.
		for (double y = 10.0; y < 11.0; y += 0.05) {
			MadRay.Hit hit = MadRay.cast(ground(y), 0.5, 13.0, 0.5, 0.5, 7.0, 0.5);
			int[] cell = MadRay.placementCell(hit);
			assertEquals(0.0, Math.max(0.0, Math.abs(hit.y() - (cell[1] + 0.5)) - 1.0), 1e-9);
		}
	}

	@Test
	void dominantFaceMatchesMinecraftDirectionOrder() {
		assertEquals(1, MadRay.dominantFace(0, 1, 0)); // UP
		assertEquals(0, MadRay.dominantFace(0, -1, 0)); // DOWN
		assertEquals(2, MadRay.dominantFace(0, 0.2, -0.9)); // NORTH (-Z)
		assertEquals(3, MadRay.dominantFace(0, 0.2, 0.9)); // SOUTH (+Z)
		assertEquals(4, MadRay.dominantFace(-0.9, 0.2, 0)); // WEST (-X)
		assertEquals(5, MadRay.dominantFace(0.9, 0.2, 0)); // EAST (+X)
	}
}
