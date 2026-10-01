package dev.madcraft.client.mixin;

import dev.madcraft.client.MadClient;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.entity.LivingEntityRenderer;
import net.minecraft.client.renderer.entity.state.HumanoidRenderState;
import net.minecraft.client.renderer.entity.state.LivingEntityRenderState;
import net.minecraft.world.entity.LivingEntity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Steve sits while the player drives a Mad Max car: Minecraft's riding pose (legs forward), as on a
 * boat or minecart. Mad Max draws the posed body in the car's seat.
 */
@Mixin(LivingEntityRenderer.class)
public abstract class DrivingPoseMixin {
	@Inject(method = "extractRenderState(Lnet/minecraft/world/entity/LivingEntity;Lnet/minecraft/client/renderer/entity/state/LivingEntityRenderState;F)V", at = @At("TAIL"))
	private void madcraft$sitInCar(LivingEntity entity, LivingEntityRenderState state, float partialTick, CallbackInfo ci) {
		if (entity == Minecraft.getInstance().player && state instanceof HumanoidRenderState humanoid && MadClient.driving()) {
			humanoid.isPassenger = true;
		}
	}
}
