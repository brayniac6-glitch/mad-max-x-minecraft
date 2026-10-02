package dev.madcraft.client;

import dev.madcraft.MadCraft;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.client.Minecraft;
import net.minecraft.client.User;
import net.minecraft.client.gui.components.toasts.SystemToast;
import net.minecraft.network.chat.Component;

/**
 * MadCraft needs a bought copy of Minecraft: Java Edition, from any launcher (official, Prism, MultiMC,
 * ATLauncher, Modrinth App, CurseForge...). The player's login is checked with Mojang's own service, the
 * way Minecraft checks it when joining a server: a Microsoft account that owns the game has a Minecraft
 * profile there; an offline or cracked session has none. Once confirmed, the account is remembered, so
 * playing without internet still works. Development runs (gradle runClient) are exempt.
 */
public final class Ownership {
	private static final String PROFILE_URL = "https://api.minecraftservices.com/minecraft/profile";

	private enum Result { PENDING, OWNED, NOT_OWNED }

	private static volatile Result result;
	private static CompletableFuture<Void> checking;
	private static boolean told;

	private Ownership() {
	}

	/** True once the player's copy is confirmed; false while checking or when it isn't. */
	public static boolean check(Minecraft minecraft) {
		if (result == null) {
			start(minecraft.getUser());
		}
		if (result == Result.NOT_OWNED && !told && minecraft.gui != null && minecraft.gui.screen() != null) {
			told = true;
			SystemToast.add(minecraft.gui.toastManager(), SystemToast.SystemToastId.PERIODIC_NOTIFICATION, Component.literal("MadCraft is off"),
				Component.literal("Sign in with a Microsoft account that owns Minecraft: Java Edition."));
		}
		return result == Result.OWNED;
	}

	private static void start(User user) {
		if (FabricLoader.getInstance().isDevelopmentEnvironment()) {
			result = Result.OWNED;
			return;
		}
		String token = user == null ? null : user.getAccessToken();
		UUID id = user == null ? null : user.getProfileId();
		if (token == null || token.length() < 20 || id == null) {
			deny("no Microsoft login (offline account)");
			return;
		}
		result = Result.PENDING;
		HttpClient client = HttpClient.newBuilder().connectTimeout(Duration.ofSeconds(10)).build();
		HttpRequest request = HttpRequest.newBuilder(URI.create(PROFILE_URL)).timeout(Duration.ofSeconds(15))
			.header("Authorization", "Bearer " + token).GET().build();
		checking = client.sendAsync(request, HttpResponse.BodyHandlers.ofString()).handle((response, error) -> {
			if (error != null) {
				// No connection: fine if this account was confirmed before on this PC.
				if (id.toString().equals(remembered())) {
					result = Result.OWNED;
					MadCraft.LOG.info("MadCraft: offline; this Minecraft account was confirmed before");
				} else {
					deny("couldn't reach Mojang to confirm the account (" + error.getClass().getSimpleName() + "); connect once to the internet");
				}
				return null;
			}
			String body = response.body() == null ? "" : response.body();
			String plainId = id.toString().replace("-", "");
			if (response.statusCode() == 200 && body.contains("\"" + plainId + "\"")) {
				result = Result.OWNED;
				remember(id.toString());
				MadCraft.LOG.info("MadCraft: Minecraft: Java Edition ownership confirmed");
			} else {
				deny("this account has no Minecraft: Java Edition profile (HTTP " + response.statusCode() + ")");
			}
			return null;
		});
	}

	private static void deny(String why) {
		result = Result.NOT_OWNED;
		MadCraft.LOG.warn("MadCraft: off: {}", why);
	}

	private static Path rememberFile() {
		return FabricLoader.getInstance().getConfigDir().resolve("madcraft-owner.txt");
	}

	private static String remembered() {
		try {
			return Files.readString(rememberFile(), StandardCharsets.UTF_8).trim();
		} catch (Exception e) {
			return "";
		}
	}

	private static void remember(String id) {
		try {
			Files.writeString(rememberFile(), id, StandardCharsets.UTF_8);
		} catch (Exception e) {
			MadCraft.LOG.warn("MadCraft: couldn't remember the confirmed account: {}", e.toString());
		}
	}
}
