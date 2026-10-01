package dev.madcraft.client;

import dev.madcraft.MadCraft;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.client.Minecraft;
import net.minecraft.client.User;
import net.minecraft.client.gui.components.toasts.SystemToast;
import net.minecraft.network.chat.Component;

/**
 * MadCraft needs a bought copy of Minecraft: Java Edition. It only links up with Mad Max when the
 * game was started by the official launcher with a Microsoft account (which carries an Xbox user
 * id; offline and cracked sessions don't). Development runs (gradle runClient) are exempt.
 */
public final class Ownership {
	private static Boolean owned;
	private static boolean told;

	private Ownership() {
	}

	public static boolean check(Minecraft minecraft) {
		if (owned == null) {
			owned = decide(minecraft.getUser());
			if (!owned) {
				MadCraft.LOG.warn("MadCraft: this Minecraft isn't signed in with a Microsoft account that owns the game; MadCraft stays off");
			}
		}
		if (!owned && !told && minecraft.gui != null && minecraft.gui.screen() != null) {
			told = true;
			SystemToast.add(minecraft.gui.toastManager(), SystemToast.SystemToastId.PERIODIC_NOTIFICATION, Component.literal("MadCraft is off"),
				Component.literal("Sign in with a Microsoft account that owns Minecraft (official launcher)."));
		}
		return owned;
	}

	private static boolean decide(User user) {
		if (FabricLoader.getInstance().isDevelopmentEnvironment()) {
			return true;
		}
		if (user == null || user.getProfileId() == null) {
			return false;
		}
		String token = user.getAccessToken();
		return user.getXuid().filter(x -> !x.isBlank()).isPresent() && token != null && token.length() > 20;
	}
}
