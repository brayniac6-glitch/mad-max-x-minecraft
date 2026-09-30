package dev.madcraft.client.mixin;

import dev.madcraft.client.MadClient;
import net.minecraft.client.Minecraft;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Minecraft.class)
public abstract class MinecraftMixin {
	@Inject(method = "runTick", at = @At("HEAD"))
	private void madcraft$beginFrame(boolean advanceGameTime, CallbackInfo ci) {
		MadClient.beginFrame();
	}

	@Inject(
		method = "renderFrame",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/GameRenderer;render()V", shift = At.Shift.AFTER)
	)
	private void madcraft$afterRender(boolean advanceGameTime, CallbackInfo ci) {
		MadClient.afterRender();
	}

	@Inject(method = "renderFrame", at = @At("TAIL"))
	private void madcraft$pace(boolean advanceGameTime, CallbackInfo ci) {
		MadClient.paceFrame();
	}
}
