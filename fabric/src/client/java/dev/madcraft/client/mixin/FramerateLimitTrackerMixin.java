package dev.madcraft.client.mixin;

import com.mojang.blaze3d.platform.FramerateLimitTracker;
import dev.madcraft.client.MadClient;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** MadClient.paceFrame() locks us to Mad Max's frame rate; don't let MC throttle on its own. */
@Mixin(FramerateLimitTracker.class)
public abstract class FramerateLimitTrackerMixin {
	@Inject(method = "getFramerateLimit", at = @At("HEAD"), cancellable = true)
	private void madcraft$unlimited(CallbackInfoReturnable<Integer> cir) {
		if (MadClient.linked()) {
			cir.setReturnValue(260);
		}
	}
}
