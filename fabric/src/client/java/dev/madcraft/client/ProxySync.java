package dev.madcraft.client;

import dev.madcraft.combat.MadMaxActorEntity;
import dev.madcraft.link.MadLink;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import net.minecraft.client.Minecraft;
import net.minecraft.world.entity.Entity;

/**
 * Puts the client's copies of the MadMax actor stand-ins exactly where MadMax has the actors this
 * frame, so the crosshair and melee reach line up with what's on screen (the server copy only
 * moves once per tick and reaches the client a tick or two later).
 */
final class ProxySync {
	private static final List<MadLink.Actor> ACTORS = new ArrayList<>();
	private static final Map<Integer, MadLink.Actor> BY_ID = new HashMap<>();

	private ProxySync() {
	}

	static void frame(Minecraft minecraft) {
		if (minecraft.level == null || !MadLink.readActors(ACTORS)) {
			return;
		}
		BY_ID.clear();
		for (MadLink.Actor a : ACTORS) {
			BY_ID.put(a.formId(), a);
		}
		for (Entity entity : minecraft.level.entitiesForRendering()) {
			if (entity instanceof MadMaxActorEntity proxy) {
				MadLink.Actor a = BY_ID.get(proxy.formId());
				if (a == null) {
					continue;
				}
				proxy.setSize(a.width(), a.height());
				proxy.setPos(a.x(), a.y(), a.z());
				proxy.xo = a.x();
				proxy.yo = a.y();
				proxy.zo = a.z();
				proxy.setYRot(a.yaw());
				proxy.yRotO = a.yaw();
			}
		}
	}
}
