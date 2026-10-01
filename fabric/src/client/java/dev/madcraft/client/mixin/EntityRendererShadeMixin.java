package dev.madcraft.client.mixin;

import dev.madcraft.client.MadClient;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.entity.EntityRenderer;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.Entity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Mad Max's darkness on the player: the sky light Minecraft lights its own player with (Steve as
 * Mad Max draws him, the first-person hand and held items) is capped by how lit Mad Max's frame is
 * around them. Minecraft's world is open sky, so without this the player would glow in Mad Max's
 * bunkers and nights. Block light (torches, placed or held) is left alone.
 */
@Mixin(EntityRenderer.class)
public abstract class EntityRendererShadeMixin {
	@Inject(method = "getSkyLightLevel", at = @At("RETURN"), cancellable = true)
	private void madcraft$madMaxShade(Entity entity, BlockPos pos, CallbackInfoReturnable<Integer> cir) {
		if (entity != Minecraft.getInstance().player) {
			return;
		}
		int cap = MadClient.playerSkyLight();
		if (cap < cir.getReturnValueI()) {
			cir.setReturnValue(cap);
		}
	}
}
