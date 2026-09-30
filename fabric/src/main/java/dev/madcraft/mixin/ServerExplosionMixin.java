package dev.madcraft.mixin;

import dev.madcraft.link.Proto;
import dev.madcraft.link.MadLink;
import net.minecraft.world.level.ServerExplosion;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Tells MadMax about every Minecraft explosion (TNT, creepers, beds, ...) once it has gone off, so
 * Mad Max's own physics feel it: loose objects are thrown and people are knocked away.
 */
@Mixin(ServerExplosion.class)
public abstract class ServerExplosionMixin {
	@Inject(method = "explode", at = @At("RETURN"))
	private void madcraft$tellMadMax(CallbackInfoReturnable<Integer> cir) {
		if (!MadLink.active()) {
			return;
		}
		ServerExplosion self = (ServerExplosion) (Object) this;
		var center = self.center();
		MadLink.pushEvent(Proto.EV_EXPLOSION, 0, (float) center.x, (float) center.y, (float) center.z, self.radius(), 0);
	}
}
