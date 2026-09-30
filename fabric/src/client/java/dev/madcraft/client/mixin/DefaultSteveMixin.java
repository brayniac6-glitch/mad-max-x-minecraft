package dev.madcraft.client.mixin;

import net.minecraft.client.player.AbstractClientPlayer;
import net.minecraft.client.resources.DefaultPlayerSkin;
import net.minecraft.world.entity.player.PlayerSkin;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Every player is the classic default Steve (wide arms) in the wasteland, whatever skin their
 * account has. This covers the body Mad Max draws too: AvatarExporter captures what Minecraft's own
 * renderer draws, and that asks the player for its skin here. Off with -Dmadcraft.forceSteve=false.
 */
@Mixin(AbstractClientPlayer.class)
public abstract class DefaultSteveMixin {
	@Unique
	private static final boolean FORCE_STEVE = Boolean.parseBoolean(System.getProperty("madcraft.forceSteve", "true"));
	@Unique
	private static PlayerSkin madcraft$steve;

	@Inject(method = "getSkin", at = @At("HEAD"), cancellable = true)
	private void madcraft$defaultSteve(CallbackInfoReturnable<PlayerSkin> cir) {
		if (FORCE_STEVE) {
			cir.setReturnValue(madcraft$steve());
		}
	}

	// DefaultPlayerSkin.getDefaultSkin() is the slim-armed Steve; the classic one is "wide/steve".
	@Unique
	private static PlayerSkin madcraft$steve() {
		if (madcraft$steve == null) {
			PlayerSkin found = DefaultPlayerSkin.getDefaultSkin();
			for (PlayerSkin skin : DefaultPlayerSkinAccessor.madcraft$defaultSkins()) {
				if (skin.body().texturePath().getPath().endsWith("wide/steve.png") || skin.body().texturePath().getPath().endsWith("wide/steve")) {
					found = skin;
					break;
				}
			}
			madcraft$steve = found;
		}
		return madcraft$steve;
	}
}
