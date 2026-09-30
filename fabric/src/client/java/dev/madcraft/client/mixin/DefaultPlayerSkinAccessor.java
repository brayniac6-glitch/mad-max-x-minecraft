package dev.madcraft.client.mixin;

import net.minecraft.client.resources.DefaultPlayerSkin;
import net.minecraft.world.entity.player.PlayerSkin;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(DefaultPlayerSkin.class)
public interface DefaultPlayerSkinAccessor {
	@Accessor("DEFAULT_SKINS")
	static PlayerSkin[] madcraft$defaultSkins() {
		throw new AssertionError();
	}
}
