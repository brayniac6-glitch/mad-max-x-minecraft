package dev.madcraft.client.mixin;

import com.mojang.blaze3d.platform.InputConstants;
import com.mojang.blaze3d.platform.Window;
import dev.madcraft.client.InputBridge;
import dev.madcraft.client.MadClient;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Keyboard state and mouse capture come from MadMax while linked, not from SDL. */
@Mixin(InputConstants.class)
public abstract class InputConstantsMixin {
	@Inject(method = "isKeyDown", at = @At("HEAD"), cancellable = true)
	private static void madcraft$isKeyDown(int key, CallbackInfoReturnable<Boolean> cir) {
		if (MadClient.tookOver()) {
			cir.setReturnValue(InputBridge.isKeyDown(key));
		}
	}

	@Inject(method = "grabMouse", at = @At("HEAD"), cancellable = true)
	private static void madcraft$grabMouse(Window window, double xpos, double ypos, CallbackInfo ci) {
		if (MadClient.tookOver()) {
			ci.cancel();
		}
	}

	@Inject(method = "releaseMouse", at = @At("HEAD"), cancellable = true)
	private static void madcraft$releaseMouse(Window window, double xpos, double ypos, CallbackInfo ci) {
		if (MadClient.tookOver()) {
			ci.cancel();
		}
	}
}
