package dev.madcraft;

import dev.madcraft.link.MadLink;
import dev.madcraft.link.Proto;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.level.gamerules.GameRules;

/**
 * One playthrough across both games: Minecraft's clock follows Mad Max's, its keepInventory rule
 * follows MadCraft.ini, and Max's death takes Minecraft's player with him (once per death; Minecraft's
 * own death already kills Max). Respawning is Minecraft's immediate respawn, after which the player is
 * put wherever Mad Max put Max (MadClient's teleport).
 */
public final class MadSurvival {
	private static final MadLink.MadState STATE = new MadLink.MadState();
	private static long lastTicks = Long.MIN_VALUE;
	private static boolean maxWasDead;

	private MadSurvival() {
	}

	public static void init() {
		ServerTickEvents.END_SERVER_TICK.register(MadSurvival::tick);
	}

	private static void tick(MinecraftServer server) {
		if (!MadLink.active() || !MadLink.readSkyState(STATE) || !STATE.inGame()) {
			return;
		}
		// Mad Max's hour -> Minecraft's day time (0 = 6:00, 6000 = noon, 18000 = midnight).
		float hour = STATE.gameHour;
		if (Float.isFinite(hour)) {
			long ticks = Math.floorMod((long) Math.round((hour - 6.0F) * 1000.0F), 24000L);
			if (lastTicks == Long.MIN_VALUE || Math.abs(ticks - lastTicks) >= 20 && Math.abs(ticks - lastTicks) <= 23980) {
				lastTicks = ticks;
				server.getCommands().performPrefixedCommand(server.createCommandSourceStack().withSuppressedOutput(), "time set " + ticks);
			}
		}
		boolean keep = (STATE.flags & Proto.SKY_KEEP_INVENTORY) != 0;
		if (server.getGameRules().get(GameRules.KEEP_INVENTORY) != keep) {
			server.getGameRules().set(GameRules.KEEP_INVENTORY, keep, server);
			MadCraft.LOG.info("MadCraft: keepInventory {}", keep);
		}
		// Max died in Mad Max: Minecraft's player dies with him (not a player that just respawned).
		boolean maxDead = (STATE.flags & Proto.SKY_PLAYER_DEAD) != 0;
		if (maxDead && !maxWasDead) {
			for (ServerPlayer player : server.getPlayerList().getPlayers()) {
				if (dev.madcraft.net.MadNet.isHost(player) && player.isAlive() && player.tickCount > 100) {
					MadCraft.LOG.info("MadCraft: Max died; so does the Minecraft player");
					player.hurtServer(player.level(), player.level().damageSources().genericKill(), Float.MAX_VALUE);
				}
			}
		}
		maxWasDead = maxDead;
	}

	/** Max drank or ate in Mad Max: the same share of Minecraft's hunger bar. */
	public static void feed(ServerPlayer player, float food, float saturation) {
		if (!player.isAlive() || food <= 0.0F) {
			return;
		}
		int points = Math.max(1, Math.round(food));
		float modifier = points > 0 ? Math.min(1.0F, saturation / (2.0F * points)) : 0.0F;
		int before = player.getFoodData().getFoodLevel();
		player.getFoodData().eat(points, modifier);
		MadCraft.LOG.info("MadCraft: Max ate/drank: hunger {} -> {}", before, player.getFoodData().getFoodLevel());
	}
}
