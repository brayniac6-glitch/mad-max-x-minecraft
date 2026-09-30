package dev.madcraft.mixin;

import dev.madcraft.MadCraft;
import dev.madcraft.combat.MadCombat;
import dev.madcraft.combat.MadMaxActorEntity;
import dev.madcraft.link.Proto;
import dev.madcraft.link.MadLink;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.server.level.ServerPlayer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(ServerPlayer.class)
public abstract class ServerPlayerMixin {
	/** Critical hits on a MadMax actor are flagged so MadMax can play them up. */
	@Inject(method = "crit", at = @At("HEAD"))
	private void madcraft$critMadMax(Entity entity, CallbackInfo ci) {
		if (entity instanceof MadMaxActorEntity proxy) {
			proxy.markCritical();
		}
	}

	/** Dying in Minecraft is dying in MadMax: the host's through the link, a guest's through theirs. */
	@Inject(method = "die", at = @At("HEAD"))
	private void madcraft$diesInMadMax(DamageSource source, CallbackInfo ci) {
		ServerPlayer self = (ServerPlayer) (Object) this;
		int attacker = MadCombat.attackerFormId(source);
		if (!dev.madcraft.net.MadNet.isHost(self)) {
			if (net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking.canSend(self, dev.madcraft.net.MadNet.Died.TYPE)) {
				net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking.send(self, new dev.madcraft.net.MadNet.Died(attacker));
			}
			MadCraft.LOG.info("MadCraft: guest {} died ({}); telling their MadMax", self.getPlainTextName(), source.getMsgId());
			return;
		}
		if (MadLink.active()) {
			MadLink.pushEvent(Proto.EV_PLAYER_DIED, attacker, 0, 0, 0, 0, 0);
			MadCraft.LOG.info("MadCraft: player died ({}); telling MadMax", source.getMsgId());
		}
	}
}
