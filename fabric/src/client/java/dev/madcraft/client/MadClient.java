package dev.madcraft.client;

import dev.madcraft.MadCraft;
import dev.madcraft.client.render.WorldExporter;
import dev.madcraft.link.Proto;
import dev.madcraft.link.MadLink;
import dev.madcraft.world.MadCollision;
import net.minecraft.client.Camera;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.phys.Vec3;
import org.lwjgl.sdl.SDLVideo;

/**
 * Per-frame glue between the Minecraft client and MadMax. Everything here runs on the render
 * thread, called from MinecraftMixin.
 */
public final class MadClient {
	private static final boolean SHOW_WINDOW = Boolean.getBoolean("madcraft.showWindow");
	// Started by MadMax (MadCraft's bundled instance passes -Dmadcraft.startHidden=true): no window and
	// no title-screen music from the first frame, even while MadMax is paused (Alt-Tabbed) and the
	// two haven't linked up yet. Otherwise the window only goes once MadMax is there.
	private static final boolean START_HIDDEN = Boolean.getBoolean("madcraft.startHidden");
	private static boolean startedHidden;

	private static final MadLink.MadState sky = new MadLink.MadState();
	private static final MadLink.McState mc = new MadLink.McState();
	private static volatile boolean linked;
	private static boolean tookOver;
	private static boolean windowHidden;
	private static int appliedViewportW, appliedViewportH;

	// Teleport / hold state: MadMax decides where the player is after loads, doors and respawns.
	private static int lastTeleportSeq = -1;
	private static int teleportAck;
	private static boolean teleportPending;
	private static LocalPlayer lastPlayer;
	private static Vec3 holdPos;
	private static Vec3 unlinkedHold;
	private static long holdSince;
	private static long qpcFreq;
	private static LocalPlayer eyePlayer;
	private static float eyeSmoothed;
	private static long frameCounter;
	private static int lastPacedSeq;
	private static boolean madmaxStalled;
	private static int exporterErrors;

	private MadClient() {
	}

	public static boolean linked() {
		return linked;
	}

	/**
	 * True once MadMax has connected in this session. From then on Minecraft never touches the
	 * real mouse or keyboard again (even if MadMax closes), since its window is hidden.
	 */
	public static boolean tookOver() {
		return tookOver;
	}

	public static MadLink.MadState sky() {
		return sky;
	}

	/** Start of Minecraft.runTick: pull state and input from MadMax before anything else runs. */
	public static void beginFrame() {
		MadLink.poll();
		quitWithMadMax(Minecraft.getInstance());
		if (START_HIDDEN && !startedHidden) {
			startedHidden = true;
			Minecraft minecraft = Minecraft.getInstance();
			hideWindowOnce(minecraft);
			minecraft.options.getSoundSourceOptionInstance(net.minecraft.sounds.SoundSource.MUSIC).set(0.0);
			minecraft.getMusicManager().stopPlaying();
		}
		boolean nowLinked = MadLink.active();
		if (nowLinked) {
			MadLink.readSkyState(sky); // on a torn read we simply keep last frame's state
			dev.madcraft.world.MadWater.refresh();
		} else {
			dev.madcraft.world.MadWater.clear();
		}
		if (nowLinked != linked) {
			linked = nowLinked;
			MadCraft.LOG.info("MadCraft: MadMax link {}", linked ? "up" : "down");
			if (linked) {
				tookOver = true;
				unlinkedHold = null;
				MadCollision.startConsumer();
				applyLinkedOptions();
			} else {
				InputBridge.releaseAll();
				LocalPlayer player = Minecraft.getInstance().player;
				unlinkedHold = player != null ? player.position() : null;
			}
		}
		if (!linked) {
			return;
		}

		Minecraft minecraft = Minecraft.getInstance();
		hideWindowOnce(minecraft);
		applyViewportSize(minecraft);
		MirrorWorld.openWhenReady(minecraft);

		if (sky.menuOpen() || sky.loading()) {
			InputBridge.releaseAll();
		}
		InputBridge.drain(minecraft);
		ProxySync.frame(minecraft);

		LocalPlayer player = minecraft.player;
		if (player == null) {
			lastPlayer = null;
			return;
		}

		// A new player object means we just joined or respawned: put it where Mad Max's player is.
		if (player != lastPlayer) {
			lastPlayer = player;
			teleportPending = true;
		}
		if (sky.teleportSeq != lastTeleportSeq) {
			lastTeleportSeq = sky.teleportSeq;
			teleportPending = true;
		}
		if (teleportPending && sky.inGame() && !sky.loading()) {
			requestTeleport(minecraft, sky.x, sky.y, sky.z, sky.yaw, sky.pitch);
			teleportAck = sky.teleportSeq;
			teleportPending = false;
			holdPos = new Vec3(sky.x, sky.y, sky.z);
		}

		// Look direction is driven by MadMax (zero-latency camera); MC uses it for everything else.
		if (minecraft.gui.screen() == null) {
			player.setYRot(sky.yaw);
			player.setXRot(sky.pitch);
			player.yRotO = sky.yaw;
			player.xRotO = sky.pitch;
		}
	}

	// Minecraft is started with MadMax (the SKSE plugin launches it), so it goes when that MadMax has
	// closed for good: saved and shut down the normal way. -Dmadcraft.quitWithMadMax=false keeps it
	// running instead (development: restarting MadMax without restarting Minecraft).
	private static final boolean QUIT_WITH_MADMAX = Boolean.parseBoolean(System.getProperty("madcraft.quitWithMadMax", "true"));
	private static long madmaxGoneSince;
	private static long nextMadMaxCheck;

	private static void quitWithMadMax(Minecraft minecraft) {
		int pid = MadLink.madmaxPid();
		long now = System.currentTimeMillis();
		if (!QUIT_WITH_MADMAX || pid == 0 || now < nextMadMaxCheck) {
			return;
		}
		nextMadMaxCheck = now + 1000;
		if (ProcessHandle.of(pid).map(ProcessHandle::isAlive).orElse(false)) {
			madmaxGoneSince = 0;
			return;
		}
		if (madmaxGoneSince == 0) {
			madmaxGoneSince = now;
		} else if (now - madmaxGoneSince > 5000) {
			MadCraft.LOG.info("MadCraft: MadMax (pid {}) has closed; saving and quitting", pid);
			minecraft.stop();
		}
	}

	/** Called at the end of every client tick. */
	public static void clientTick(Minecraft minecraft) {
		MirrorWorld.tick(minecraft);
		DiscordPresence.tick(minecraft);
		freezeWhileUnlinked(minecraft);
		holdUntilReady(minecraft);
		publishTick(minecraft);
		diagnose(minecraft);
	}

	// Every 2 s of wall time while linked and moving: is the player slow because Minecraft ticks
	// slowly, or because something in its own movement slows it?
	private static long diagSince;
	private static int diagTicks;
	private static Vec3 diagFrom;

	private static void diagnose(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (!linked || player == null) {
			diagSince = 0;
			return;
		}
		long now = System.currentTimeMillis();
		if (diagSince == 0) {
			diagSince = now;
			diagTicks = 0;
			diagFrom = player.position();
			return;
		}
		diagTicks++;
		if (now - diagSince < 2000) {
			return;
		}
		double seconds = (now - diagSince) / 1000.0;
		double moved = Math.hypot(player.getX() - diagFrom.x, player.getZ() - diagFrom.z);
		boolean forward = minecraft.options.keyUp.isDown();
		if (moved > 0.01 || forward) {
			var move = player.input.getMoveVector();
			var speed = player.getAttribute(net.minecraft.world.entity.ai.attributes.Attributes.MOVEMENT_SPEED);
			MadCraft.LOG.info("MadCraft: diag {} ticks/s, {} fps, {} blocks/s, forward key {}, move ({}, {}), velocity {}, ground {}, usingItem {}, crouching {}, sprinting {}, speed attr {}",
				String.format("%.1f", diagTicks / seconds), minecraft.getFps(), String.format("%.2f", moved / seconds), forward,
				String.format("%.2f", move.x), String.format("%.2f", move.y), player.getDeltaMovement(), player.onGround(), player.isUsingItem(),
				player.isCrouching(), player.isSprinting(), speed == null ? "?" : String.format("%.3f", speed.getValue()));
		}
		diagSince = now;
		diagTicks = 0;
		diagFrom = player.position();
	}

	/**
	 * MadMax went quiet (a long loading screen, a stall, or it closed). Its collision around the
	 * player may be about to change (interior doors), so keep the player exactly where they were
	 * instead of letting them fall; MadMax puts them where they belong when it's back.
	 */
	private static void freezeWhileUnlinked(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (linked || !tookOver || player == null) {
			return;
		}
		if (unlinkedHold == null) {
			unlinkedHold = player.position();
		}
		player.setDeltaMovement(Vec3.ZERO);
		player.setPos(unlinkedHold.x, unlinkedHold.y, unlinkedHold.z);
		player.xo = unlinkedHold.x;
		player.yo = unlinkedHold.y;
		player.zo = unlinkedHold.z;
		player.resetFallDistance();
	}

	/**
	 * Hands MadMax the raw physics tick (previous + latest feet, smoothed eye height, walk bob) with a
	 * QueryPerformanceCounter timestamp. MadMax interpolates between them on its own frame clock,
	 * exactly like Minecraft's renderer does with partial ticks.
	 */
	private static void publishTick(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (!linked || player == null) {
			return;
		}
		if (qpcFreq == 0) {
			qpcFreq = MadLink.qpcFrequency();
		}
		float tickMs = minecraft.level != null ? minecraft.level.tickRateManager().millisecondsPerTick() : 50.0F;
		// The tick really "happened" partial ticks ago (DeltaTracker keeps the remainder).
		float remainder = minecraft.getDeltaTracker().getGameTimeDeltaPartialTick(false);
		mc.tickQpc = MadLink.qpc() - (long) (remainder * tickMs * qpcFreq / 1000.0);
		mc.tickMs = tickMs;
		mc.prevX = player.xo;
		mc.prevY = player.yo;
		mc.prevZ = player.zo;
		mc.curX = player.getX();
		mc.curY = player.getY();
		mc.curZ = player.getZ();
		// Same smoothing as Camera.tick(): eye height eases halfway toward the target each tick.
		if (player != eyePlayer) {
			eyePlayer = player;
			eyeSmoothed = player.getEyeHeight();
		}
		mc.eyeHeightO = eyeSmoothed;
		eyeSmoothed += (player.getEyeHeight() - eyeSmoothed) * 0.5F;
		mc.eyeHeightT = eyeSmoothed;
		boolean bob = minecraft.options.bobView().get();
		var avatar = player.avatarState();
		mc.walkDistO = bob ? avatar.getInterpolatedWalkDistance(0.0F) : 0.0F;
		mc.walkDist = bob ? avatar.getInterpolatedWalkDistance(1.0F) : 0.0F;
		mc.bobO = bob ? avatar.getInterpolatedBob(0.0F) : 0.0F;
		mc.bob = bob ? avatar.getInterpolatedBob(1.0F) : 0.0F;
		MadLink.writeMcState(mc);
	}

	/** Freeze the player until Mad Max's collision around them has arrived. */
	private static void holdUntilReady(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (!linked || player == null) {
			return;
		}
		if (!sky.inGame() || sky.loading()) {
			// MadMax is on its main menu or a loading screen: park the player where they are.
			if (holdPos == null) {
				holdPos = player.position();
			}
			teleportPending = true;
		}
		if (holdPos == null) {
			holdSince = 0;
			return;
		}
		if (holdSince == 0) {
			holdSince = System.currentTimeMillis();
		}
		int bx = (int) Math.floor(holdPos.x), by = (int) Math.floor(holdPos.y), bz = (int) Math.floor(holdPos.z);
		boolean known = MadCollision.isKnown(bx, by - 1, bz) && MadCollision.isKnown(bx, by, bz)
			&& MadCollision.isKnown(bx, by - MadCollision.REGION_SIZE, bz);
		// Release once there is actual ground below (or after a timeout, e.g. when mid-air on purpose).
		boolean ready = known && (MadCollision.hasSolidBelow(bx, by, bz, 12) || System.currentTimeMillis() - holdSince > 6000);
		if (ready && sky.inGame() && !sky.loading()) {
			// Mad Max's feet can sit a fraction of a voxel inside our ground layer. Minecraft's
			// collision never pushes you out of a shape, so you'd drop through: lift out first.
			Vec3 safe = liftOutOfGeometry(player, holdPos);
			if (safe.y != holdPos.y) {
				player.setPos(safe.x, safe.y, safe.z);
				player.yo = safe.y;
				MadCraft.LOG.info("MadCraft: lifted player {} blocks out of the ground", String.format("%.3f", safe.y - holdPos.y));
			}
			holdPos = null;
			return;
		}
		player.setDeltaMovement(Vec3.ZERO);
		player.setPos(holdPos.x, holdPos.y, holdPos.z);
		player.xo = holdPos.x;
		player.yo = holdPos.y;
		player.zo = holdPos.z;
		player.resetFallDistance();
	}

	private static Vec3 liftOutOfGeometry(LocalPlayer player, Vec3 pos) {
		// Stand on the exact MadMax ground if it is slightly above the feet (up to 2.5 blocks).
		double ground = MadCollider.groundAt(pos.x, pos.y, pos.z, 2.5);
		return !Double.isNaN(ground) && ground > pos.y ? new Vec3(pos.x, ground, pos.z) : pos;
	}

	private static void requestTeleport(Minecraft minecraft, double x, double y, double z, float yaw, float pitch) {
		LocalPlayer player = minecraft.player;
		player.setPos(x, y, z);
		player.setDeltaMovement(Vec3.ZERO);
		player.resetFallDistance();
		var server = minecraft.getSingleplayerServer();
		if (server != null) {
			var uuid = player.getUUID();
			server.execute(() -> {
				ServerPlayer sp = server.getPlayerList().getPlayer(uuid);
				if (sp != null) {
					sp.teleportTo(x, y, z);
					sp.setYRot(yaw);
					sp.setXRot(pitch);
					sp.resetFallDistance();
				}
			});
		}
		MadCraft.LOG.info("MadCraft: teleported to {} {} {}", x, y, z);
	}

	/** After GameRenderer.render(): report the player to MadMax and ship the overlay frame. */
	public static void afterRender() {
		if (!linked) {
			return;
		}
		Minecraft minecraft = Minecraft.getInstance();
		LocalPlayer player = minecraft.player;
		int flags = 0;
		if (player != null && minecraft.level != null) {
			float partial = minecraft.getDeltaTracker().getGameTimeDeltaPartialTick(false);
			Vec3 feet = player.getPosition(partial);
			Camera camera = minecraft.gameRenderer.mainCamera();
			flags |= Proto.MC_IN_WORLD;
			if (player.onGround()) {
				flags |= Proto.MC_ON_GROUND;
			}
			if (player.isShiftKeyDown()) {
				flags |= Proto.MC_SNEAKING;
			}
			if (player.isSprinting()) {
				flags |= Proto.MC_SPRINTING;
			}
			if (player.isDeadOrDying()) {
				flags |= Proto.MC_DEAD;
			}
			if (player.isSwimming()) {
				flags |= Proto.MC_SWIMMING;
			}
			if (player.getAbilities().flying) {
				flags |= Proto.MC_FLYING;
			}
			mc.x = feet.x;
			mc.y = feet.y;
			mc.z = feet.z;
			mc.yaw = player.getYRot();
			mc.pitch = player.getXRot();
			// The eye, not the camera: in third person Minecraft's camera sits behind or in front.
			Vec3 eye = camera.isDetached() ? player.getEyePosition(partial) : camera.position();
			mc.eyeHeight = (float) (eye.y - feet.y);
			mc.eyeX = eye.x;
			mc.eyeY = eye.y;
			mc.eyeZ = eye.z;
			mc.fov = camera.getFov();
			// Minecraft's F5 camera: MadMax puts its camera where Minecraft's would be.
			mc.cameraMode = minecraft.options.getCameraType().ordinal();
			mc.cameraDistance = camera.isDetached() ? (float) camera.position().distanceTo(player.getEyePosition(partial)) : 0.0F;
			// Walk bob, exactly what GameRenderer.bobView() uses this frame.
			var entityState = minecraft.gameRenderer.gameRenderState().levelRenderState.cameraRenderState.entityRenderState;
			boolean bob = minecraft.options.bobView().get() && entityState.isPlayer;
			mc.bobPhase = bob ? entityState.backwardsInterpolatedWalkDistance : 0.0F;
			mc.bobAmount = bob ? entityState.bob : 0.0F;
		}
		if (minecraft.gui.screen() != null) {
			flags |= Proto.MC_SCREEN_OPEN;
		}
		mc.flags = flags;
		mc.sensitivity = minecraft.options.sensitivity().get().floatValue();
		mc.teleportAck = holdPos == null ? teleportAck : teleportAck - 1; // not "arrived" until we are released
		mc.guiScale = minecraft.getWindow().getGuiScale();
		mc.frameCounter = ++frameCounter;
		MadLink.writeMcState(mc);

		if ((flags & Proto.MC_IN_WORLD) != 0) {
			try {
				WorldExporter.frame(minecraft, minecraft.getDeltaTracker().getGameTimeDeltaPartialTick(false));
			} catch (RuntimeException e) {
				if (exporterErrors++ < 5) {
					MadCraft.LOG.error("MadCraft: world export failed", e);
				}
			}
			FrameExporter.capture(minecraft);
		}
	}

	/** End of the frame: render at most once per Mad Max frame instead of spinning freely. */
	public static void paceFrame() {
		if (!linked) {
			return;
		}
		if (madmaxStalled && (MadLink.skyStateSeq() >>> 1) == lastPacedSeq) {
			return; // MadMax is paused (menu / alt-tab): don't block every frame waiting for it
		}
		madmaxStalled = false;
		long deadline = System.nanoTime() + 25_000_000L;
		// MadState.seq advances by 2 per Mad Max frame (odd while writing).
		while ((MadLink.skyStateSeq() >>> 1) == lastPacedSeq && System.nanoTime() < deadline) {
			Thread.onSpinWait();
			if (deadline - System.nanoTime() > 2_000_000L) {
				Thread.yield();
			}
		}
		int seqNow = MadLink.skyStateSeq() >>> 1;
		madmaxStalled = seqNow == lastPacedSeq;
		lastPacedSeq = seqNow;
	}

	private static void applyLinkedOptions() {
		Minecraft minecraft = Minecraft.getInstance();
		var options = minecraft.options;
		options.pauseOnLostFocus = false;
		options.vignette().set(false);
		options.enableVsync().set(false);
		options.framerateLimit().set(260);
		// Minecraft doesn't draw the world itself; these only decide how far out placed blocks,
		// arrows and Mad Max NPC stand-ins stay loaded and simulated.
		options.renderDistance().set(8);
		options.simulationDistance().set(8);
		options.autoJump().set(false);
		options.onboardAccessibility = false;
		if (options.tutorialStep != net.minecraft.client.tutorial.TutorialSteps.NONE) {
			minecraft.getTutorial().setStep(net.minecraft.client.tutorial.TutorialSteps.NONE);
		}
		options.getSoundSourceOptionInstance(net.minecraft.sounds.SoundSource.MUSIC).set(0.0);
		options.save();
	}

	private static void hideWindowOnce(Minecraft minecraft) {
		if (windowHidden || SHOW_WINDOW) {
			return;
		}
		windowHidden = true;
		SDLVideo.SDL_HideWindow(minecraft.getWindow().handle());
		MadCraft.LOG.info("MadCraft: game window hidden (run with -Dmadcraft.showWindow=true to keep it)");
	}

	// Minecraft draws its hand, HUD and screens at this fraction of Mad Max's resolution and Mad Max
	// scales them up with crisp pixels: at 0.5 Minecraft picks half the GUI scale, so its pixel art
	// lands at exactly the same size, for a quarter of the rendering and copying (smoother frames).
	private static final double OVERLAY_SCALE = Math.clamp(Double.parseDouble(System.getProperty("madcraft.overlayScale", "0.5")), 0.25, 1.0);

	private static void applyViewportSize(Minecraft minecraft) {
		int w = Math.min((int) Math.round(sky.viewportW * OVERLAY_SCALE), Proto.MAX_OVERLAY_W);
		int h = Math.min((int) Math.round(sky.viewportH * OVERLAY_SCALE), Proto.MAX_OVERLAY_H);
		if (w <= 0 || h <= 0 || (w == appliedViewportW && h == appliedViewportH)) {
			return;
		}
		appliedViewportW = w;
		appliedViewportH = h;
		minecraft.getWindow().setWindowed(w, h);
		MadCraft.LOG.info("MadCraft: sizing overlay to MadMax viewport {}x{}", w, h);
	}
}
