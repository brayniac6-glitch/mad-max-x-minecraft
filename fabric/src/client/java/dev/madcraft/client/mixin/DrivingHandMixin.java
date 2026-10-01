package dev.madcraft.client.mixin;

import dev.madcraft.client.MadClient;
import net.minecraft.client.renderer.GameRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** No first-person hand in a Mad Max car: Mad Max's own car camera shows the drive. */
@Mixin(GameRenderer.class)
public abstract class DrivingHandMixin {
	@Inject(method = "renderItemInHand", at = @At("HEAD"), cancellable = true)
	private void madcraft$noHandWhileDriving(CallbackInfo ci) {
		if (MadClient.driving()) {
			ci.cancel();
		}
	}
}
