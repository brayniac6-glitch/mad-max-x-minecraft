#include "SceneLight.h"

#include "Game.h"

#include <d3dcompiler.h>

namespace madcraft::SceneLight
{
	namespace
	{
		template <class T>
		void Release(T*& a_ptr)
		{
			if (a_ptr) {
				a_ptr->Release();
				a_ptr = nullptr;
			}
		}

		constexpr int kMaxLights = 48;

		constexpr char kShader[] = R"(
cbuffer LightFrame : register(b0)
{
	row_major float4x4 invViewProj;  // clip -> camera-relative Mad Max world (row vectors)
	float4 screen;                   // x: flip depth (z = 1 - d), y: the sky's depth, z: light count, w: seconds
	float4 tune;                     // x: intensity, y: wrap (light on faces turned away), z: albedo floor, w: scene copy bound
	float4 world;                    // x: metres per Minecraft block
	float4 lightPos[48];             // xyz: camera-relative position (m), w: Minecraft light level
	float4 lightCol[48];             // rgb, w: kind (0 steady, 1 flame, 2 lava)
};
Texture2D<float> depthTex : register(t0);
Texture2D sceneColor : register(t1);

float4 VSMain(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 At(int2 px, float d, float2 size)
{
	float2 ndc = float2((px.x + 0.5) / size.x * 2 - 1, 1 - (px.y + 0.5) / size.y * 2);
	float  z = screen.x > 0.5 ? 1 - d : d;
	float4 p = mul(float4(ndc, z, 1), invViewProj);
	return p.xyz / p.w;
}

float Curve(float l) { return l / (4.0 - 3.0 * l); }  // Minecraft's light-level falloff

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
	int2 px = int2(pos.xy);
	uint w, h;
	depthTex.GetDimensions(w, h);
	float2 size = float2(w, h);
	float d = depthTex.Load(int3(px, 0));
	if (abs(d - screen.y) < 1e-7) {
		discard;  // sky
	}
	float3 P = At(px, d, size);
	// The surface's normal from its neighbours' depth, taking the nearer side each way (edges).
	int2   pr = min(px + int2(1, 0), int2(w - 1, h - 1)), pl = max(px - int2(1, 0), 0);
	int2   pd = min(px + int2(0, 1), int2(w - 1, h - 1)), pu = max(px - int2(0, 1), 0);
	float3 R = At(pr, depthTex.Load(int3(pr, 0)), size), L = At(pl, depthTex.Load(int3(pl, 0)), size);
	float3 D = At(pd, depthTex.Load(int3(pd, 0)), size), U = At(pu, depthTex.Load(int3(pu, 0)), size);
	float3 dx = length(R - P) < length(P - L) ? R - P : P - L;
	float3 dy = length(D - P) < length(P - U) ? D - P : P - U;
	float3 n = normalize(cross(dy, dx));
	if (dot(n, P) > 0) {
		n = -n;  // face the camera (at the origin)
	}
	float3 sum = 0;
	uint count = (uint)screen.z;
	for (uint i = 0; i < count; ++i) {
		float3 toL = lightPos[i].xyz - P;
		float  dist = length(toL);
		float  blocks = dist / world.x;
		float  level = lightPos[i].w;
		if (blocks >= level) {
			continue;
		}
		// Minecraft: a light of level L lights L - distance; its brightness follows Curve().
		float b = Curve(saturate((level - blocks) / 15.0));
		float facing = lerp(tune.y, 1.0, saturate(dot(n, toL / max(dist, 1e-3))));
		float kind = lightCol[i].w, t = screen.w, seed = i * 1.731;
		float flicker = kind < 0.5 ? 1.0 : kind < 1.5 ? 0.86 + 0.14 * (0.6 * sin(t * 9.1 + seed) + 0.4 * sin(t * 23.7 + seed * 2.3))
		                                             : 0.9 + 0.1 * sin(t * 1.3 + seed);
		sum += lightCol[i].rgb * (b * facing * flicker);
	}
	if (dot(sum, 1) < 1e-4) {
		discard;
	}
	float3 scene = tune.w > 0.5 ? sceneColor.Load(int3(px, 0)).rgb : 0.25;
	float  luma = dot(scene, float3(0.2126, 0.7152, 0.0722));
	// The surface's colour, roughly: brighter where Mad Max shows it brighter, never black (a torch
	// lights a surface Mad Max left in the dark).
	float3 albedo = saturate(scene * 1.6 + tune.z);
	return float4(albedo * sum * tune.x * (1.0 - 0.7 * luma), 0);
}
)";

		struct alignas(16) LightConstants
		{
			float invViewProj[4][4];
			float screen[4];
			float tune[4];
			float world[4];
			float lightPos[kMaxLights][4];
			float lightCol[kMaxLights][4];
		};

		struct Light
		{
			double x, y, z;  // Minecraft block centre
			float  r, g, b;
			float  level;
			float  kind;
		};

		std::unordered_map<std::uint64_t, std::vector<Light>> sectionLights;

		std::uint64_t Key(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z)
		{
			return (std::uint64_t(std::uint32_t(a_x) & 0x1FFFFF) << 42) | (std::uint64_t(std::uint32_t(a_y) & 0x1FFFFF) << 21) | (std::uint32_t(a_z) & 0x1FFFFF);
		}

		// [Render]
		bool  enabled = true;
		float intensity = 1.5f;
		float range = 48.0f;  // blocks: lights further from the camera are left out
		float shadeRef = 0.3f;
		float shadeMin = 0.0f;
		bool  shadeOn = true;
		bool  settingsRead = false;

		void ReadSettings()
		{
			if (settingsRead) {
				return;
			}
			settingsRead = true;
			enabled = IniBool("Render", "bDynamicLights", true);
			intensity = static_cast<float>(IniDouble("Render", "fTorchIntensity", 1.5));
			range = static_cast<float>(IniDouble("Render", "fLightRange", 48.0));
			shadeOn = IniBool("Render", "bPlayerShade", true);
			shadeRef = std::max(0.02f, static_cast<float>(IniDouble("Render", "fShadeDaylight", 0.3)));
			shadeMin = std::clamp(static_cast<float>(IniDouble("Render", "fShadeMin", 0.0)), 0.0f, 1.0f);
			logger::info("scene light: Minecraft lights {} (intensity {}), player shade {} (daylight at frame brightness {})", enabled ? "on" : "off", intensity,
				shadeOn ? "on" : "off", shadeRef);
		}

		ID3D11Device*            dev = nullptr;
		bool                     failed = false;
		ID3D11VertexShader*      vs = nullptr;
		ID3D11PixelShader*       ps = nullptr;
		ID3D11Buffer*            cb = nullptr;
		ID3D11BlendState*        addBlend = nullptr;
		ID3D11DepthStencilState* noDepth = nullptr;
		ID3D11RasterizerState*   raster = nullptr;

		// Our own copy of the scene depth, readable by a shader (Mad Max's isn't necessarily).
		ID3D11Texture2D*          depthCopy = nullptr;
		ID3D11ShaderResourceView* depthSrv = nullptr;
		D3D11_TEXTURE2D_DESC      depthDesc{};

		bool Init(ID3D11Device* a_device)
		{
			if (dev) {
				return true;
			}
			if (failed) {
				return false;
			}
			ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *errors = nullptr;
			for (auto [entry, target, blob] : { std::tuple{ "VSMain", "vs_5_0", &vsBlob }, std::tuple{ "PSMain", "ps_5_0", &psBlob } }) {
				if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "madcraft_lights", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &errors))) {
					logger::error("scene light shader {} failed: {}", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
					Release(errors);
					Release(vsBlob);
					failed = true;
					return false;
				}
				Release(errors);
			}
			a_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
			a_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
			Release(vsBlob);
			Release(psBlob);

			D3D11_BUFFER_DESC cbd{};
			cbd.Usage = D3D11_USAGE_DYNAMIC;
			cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			cbd.ByteWidth = sizeof(LightConstants);
			a_device->CreateBuffer(&cbd, nullptr, &cb);

			D3D11_BLEND_DESC bd{};
			bd.RenderTarget[0].BlendEnable = TRUE;
			bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
			bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
			bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
			bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
			bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
			a_device->CreateBlendState(&bd, &addBlend);

			D3D11_DEPTH_STENCIL_DESC dd{};
			dd.DepthEnable = FALSE;
			dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
			a_device->CreateDepthStencilState(&dd, &noDepth);

			D3D11_RASTERIZER_DESC rd{};
			rd.FillMode = D3D11_FILL_SOLID;
			rd.CullMode = D3D11_CULL_NONE;
			a_device->CreateRasterizerState(&rd, &raster);

			if (!vs || !ps || !cb || !addBlend || !noDepth || !raster) {
				logger::error("scene light failed to initialize");
				failed = true;
				return false;
			}
			dev = a_device;
			return true;
		}

		// Depth format -> the typeless format of its family and the format a shader reads it as.
		bool DepthFormats(DXGI_FORMAT a_fmt, DXGI_FORMAT& a_typeless, DXGI_FORMAT& a_srv)
		{
			switch (a_fmt) {
			case DXGI_FORMAT_D32_FLOAT:
			case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_R32_FLOAT:
				a_typeless = DXGI_FORMAT_R32_TYPELESS;
				a_srv = DXGI_FORMAT_R32_FLOAT;
				return true;
			case DXGI_FORMAT_D24_UNORM_S8_UINT:
			case DXGI_FORMAT_R24G8_TYPELESS:
				a_typeless = DXGI_FORMAT_R24G8_TYPELESS;
				a_srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
				return true;
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
			case DXGI_FORMAT_R32G8X24_TYPELESS:
				a_typeless = DXGI_FORMAT_R32G8X24_TYPELESS;
				a_srv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
				return true;
			case DXGI_FORMAT_D16_UNORM:
			case DXGI_FORMAT_R16_TYPELESS:
				a_typeless = DXGI_FORMAT_R16_TYPELESS;
				a_srv = DXGI_FORMAT_R16_UNORM;
				return true;
			default:
				return false;
			}
		}

		bool CopyDepth(ID3D11DeviceContext* a_context, ID3D11DepthStencilView* a_dsv)
		{
			ID3D11Resource* res = nullptr;
			a_dsv->GetResource(&res);
			ID3D11Texture2D* tex = nullptr;
			if (!res || FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
				Release(res);
				return false;
			}
			Release(res);
			D3D11_TEXTURE2D_DESC td{};
			tex->GetDesc(&td);
			if (!depthCopy || td.Width != depthDesc.Width || td.Height != depthDesc.Height || td.Format != depthDesc.Format) {
				Release(depthSrv);
				Release(depthCopy);
				depthDesc = td;
				DXGI_FORMAT typeless{}, srvFormat{};
				static bool warned = false;
				if (!DepthFormats(td.Format, typeless, srvFormat) || td.SampleDesc.Count != 1) {
					if (!std::exchange(warned, true)) {
						logger::warn("scene light: can't read Mad Max's depth format {} (samples {}); torches won't light its world", static_cast<int>(td.Format), td.SampleDesc.Count);
					}
					tex->Release();
					return false;
				}
				D3D11_TEXTURE2D_DESC cd = td;
				cd.Format = typeless;
				cd.MipLevels = 1;
				cd.ArraySize = 1;
				cd.Usage = D3D11_USAGE_DEFAULT;
				cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				cd.CPUAccessFlags = 0;
				cd.MiscFlags = 0;
				D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
				sd.Format = srvFormat;
				sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				sd.Texture2D.MipLevels = 1;
				if (FAILED(dev->CreateTexture2D(&cd, nullptr, &depthCopy)) || FAILED(dev->CreateShaderResourceView(depthCopy, &sd, &depthSrv))) {
					if (!std::exchange(warned, true)) {
						logger::warn("scene light: no readable depth copy (format {})", static_cast<int>(td.Format));
					}
					Release(depthCopy);
					tex->Release();
					return false;
				}
				logger::info("scene light: reading Mad Max's scene depth (format {}, {}x{})", static_cast<int>(td.Format), td.Width, td.Height);
			}
			if (td.MipLevels == 1 && td.ArraySize == 1) {
				a_context->CopyResource(depthCopy, tex);
			} else {
				a_context->CopySubresourceRegion(depthCopy, 0, 0, 0, 0, tex, 0, nullptr);
			}
			tex->Release();
			return true;
		}

		bool Invert(const double a_m[16], double a_out[16])
		{
			double inv[16];
			const double* m = a_m;
			inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
			inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
			inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
			inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
			inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
			inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
			inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
			inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
			inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
			inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
			inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
			inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
			inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
			inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
			inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
			inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
			const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
			if (std::fabs(det) < 1e-30) {
				return false;
			}
			for (int i = 0; i < 16; ++i) {
				a_out[i] = inv[i] / det;
			}
			return true;
		}

		// ---- shade: the frame's brightness around the player, read back from a small mip --------
		constexpr int     kStaging = 3;
		ID3D11Texture2D*  staging[kStaging]{};
		bool              stagingFilled[kStaging]{};
		UINT              stagingW = 0, stagingH = 0, stagingMip = 0;
		DXGI_FORMAT       stagingFormat = DXGI_FORMAT_UNKNOWN;
		int               stagingNext = 0;
		float             sceneFactor = 1.0f;  // smoothed
		std::uint64_t     lastShadeQpc = 0;

		float Luma(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

		// Average brightness of the lower middle of the frame (what's around and under the player)
		// from a mapped mip; false for formats we can't read.
		bool Average(const D3D11_MAPPED_SUBRESOURCE& a_map, float& a_out)
		{
			const UINT x0 = stagingW / 5, x1 = std::max(x0 + 1, stagingW - stagingW / 5);
			const UINT y0 = stagingH * 3 / 10, y1 = stagingH;
			double     sum = 0.0, weight = 0.0;
			for (UINT y = y0; y < y1; ++y) {
				const auto* row = static_cast<const std::uint8_t*>(a_map.pData) + std::size_t(y) * a_map.RowPitch;
				for (UINT x = x0; x < x1; ++x) {
					float r = 0, g = 0, b = 0;
					const std::uint32_t p = reinterpret_cast<const std::uint32_t*>(row)[x];
					switch (stagingFormat) {
					case DXGI_FORMAT_R8G8B8A8_UNORM:
					case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
						r = (p & 0xFF) / 255.0f, g = (p >> 8 & 0xFF) / 255.0f, b = (p >> 16 & 0xFF) / 255.0f;
						break;
					case DXGI_FORMAT_B8G8R8A8_UNORM:
					case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
					case DXGI_FORMAT_B8G8R8X8_UNORM:
						b = (p & 0xFF) / 255.0f, g = (p >> 8 & 0xFF) / 255.0f, r = (p >> 16 & 0xFF) / 255.0f;
						break;
					case DXGI_FORMAT_R10G10B10A2_UNORM:
						r = (p & 0x3FF) / 1023.0f, g = (p >> 10 & 0x3FF) / 1023.0f, b = (p >> 20 & 0x3FF) / 1023.0f;
						break;
					default:
						return false;
					}
					// The middle counts most (where the player stands in third person).
					const float cx = (float(x) + 0.5f) / stagingW - 0.5f;
					const float wgt = 1.0f - std::fabs(cx);
					sum += Luma(r, g, b) * wgt;
					weight += wgt;
				}
			}
			if (weight <= 0.0) {
				return false;
			}
			a_out = float(sum / weight);
			return true;
		}
	}

	void OnLights(const std::uint8_t* a_data, std::uint32_t a_bytes)
	{
		if (a_bytes < sizeof(proto::RenLights)) {
			return;
		}
		const auto* hdr = reinterpret_cast<const proto::RenLights*>(a_data);
		const auto  key = Key(hdr->sx, hdr->sy, hdr->sz);
		const auto  count = std::min<std::uint64_t>(hdr->count, (a_bytes - sizeof(proto::RenLights)) / sizeof(proto::RenLight));
		if (count == 0) {
			sectionLights.erase(key);
			return;
		}
		const auto* src = reinterpret_cast<const proto::RenLight*>(a_data + sizeof(proto::RenLights));
		auto&       out = sectionLights[key];
		out.clear();
		for (std::uint64_t i = 0; i < count; ++i) {
			const auto& l = src[i];
			if (l.level == 0) {
				continue;
			}
			out.push_back({ hdr->sx * 16.0 + l.x + 0.5, hdr->sy * 16.0 + l.y + 0.5, hdr->sz * 16.0 + l.z + 0.5, (l.color & 0xFF) / 255.0f,
				(l.color >> 8 & 0xFF) / 255.0f, (l.color >> 16 & 0xFF) / 255.0f, float(l.level), float((l.color >> 24) & 0x0F) });
		}
		static int logged = 0;
		if (logged++ < 3) {
			logger::info("scene light: section ({}, {}, {}) has {} Minecraft light(s)", hdr->sx, hdr->sy, hdr->sz, out.size());
		}
	}

	void RemoveSection(std::int32_t a_sx, std::int32_t a_sy, std::int32_t a_sz)
	{
		sectionLights.erase(Key(a_sx, a_sy, a_sz));
	}

	void Clear()
	{
		sectionLights.clear();
	}

	void Apply(ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_rtv, const FrameInfo& a_frame)
	{
		ReadSettings();
		if (!enabled || !a_frame.depth || !Init(a_device)) {
			return;
		}
		// The lights nearest the camera (Minecraft's blocks and what the player holds).
		struct Near
		{
			double d2;
			Light  l;
		};
		static std::vector<Near> nearList;
		nearList.clear();
		const auto cam = MadMax::ToMc(a_frame.camPos);
		const double r2 = double(range) * range;
		for (const auto& [key, list] : sectionLights) {
			for (const auto& l : list) {
				const double dx = l.x - cam.x, dy = l.y - cam.y, dz = l.z - cam.z;
				const double d2 = dx * dx + dy * dy + dz * dz;
				if (d2 < r2) {
					nearList.push_back({ d2, l });
				}
			}
		}
		const auto& st = State();
		if (const auto held = st.heldLight.load(); (held & 0xFF) != 0 && st.mcInWorld) {
			// In the hand: at the eyes in first person, at Steve's chest when he's in view.
			double p[3] = { cam.x, cam.y - 0.3, cam.z };
			if (st.bodyValid) {
				p[0] = st.bodyX, p[1] = st.bodyY + 1.1, p[2] = st.bodyZ;
			}
			nearList.push_back({ 0.0, { p[0], p[1], p[2], (held >> 8 & 0xFF) / 255.0f, (held >> 16 & 0xFF) / 255.0f, (held >> 24 & 0xFF) / 255.0f,
										  float(held & 0xFF), 1.0f } });
		}
		if (nearList.empty()) {
			return;
		}
		std::sort(nearList.begin(), nearList.end(), [](const Near& a, const Near& b) { return a.d2 < b.d2; });
		if (nearList.size() > kMaxLights) {
			nearList.resize(kMaxLights);
		}
		if (!CopyDepth(a_context, a_frame.depth)) {
			return;
		}

		LightConstants lc{};
		double vp[16], inv[16];
		for (int r = 0; r < 4; ++r) {
			for (int c = 0; c < 4; ++c) {
				vp[r * 4 + c] = a_frame.viewProj[r][c];
			}
		}
		if (!Invert(vp, inv)) {
			return;
		}
		for (int i = 0; i < 16; ++i) {
			lc.invViewProj[i / 4][i % 4] = float(inv[i]);
		}
		static const auto t0 = std::chrono::steady_clock::now();
		lc.screen[0] = a_frame.flipDepth ? 1.0f : 0.0f;
		lc.screen[1] = a_frame.clearDepth;
		lc.screen[2] = float(nearList.size());
		lc.screen[3] = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
		lc.tune[0] = intensity;
		lc.tune[1] = 0.3f;
		lc.tune[2] = 0.12f;
		lc.tune[3] = a_frame.scene ? 1.0f : 0.0f;
		const Vec3 one = MadMax::FromMc(0.0, 1.0, 0.0), zero = MadMax::FromMc(0.0, 0.0, 0.0);
		lc.world[0] = std::max(0.01f, one.y - zero.y);
		for (std::size_t i = 0; i < nearList.size(); ++i) {
			const auto& l = nearList[i].l;
			const Vec3  w = MadMax::FromMc(l.x, l.y, l.z);
			lc.lightPos[i][0] = float(double(w.x) - a_frame.camPos.x);
			lc.lightPos[i][1] = float(double(w.y) - a_frame.camPos.y);
			lc.lightPos[i][2] = float(double(w.z) - a_frame.camPos.z);
			lc.lightPos[i][3] = l.level;
			lc.lightCol[i][0] = l.r;
			lc.lightCol[i][1] = l.g;
			lc.lightCol[i][2] = l.b;
			lc.lightCol[i][3] = l.kind;
		}
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return;
		}
		std::memcpy(mapped.pData, &lc, sizeof(lc));
		a_context->Unmap(cb, 0);

		D3D11_VIEWPORT view{ 0, 0, float(a_frame.width), float(a_frame.height), 0, 1 };
		a_context->RSSetViewports(1, &view);
		a_context->RSSetState(raster);
		a_context->OMSetRenderTargets(1, &a_rtv, nullptr);
		a_context->OMSetBlendState(addBlend, nullptr, 0xFFFFFFFF);
		a_context->OMSetDepthStencilState(noDepth, 0);
		a_context->IASetInputLayout(nullptr);
		a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		a_context->VSSetShader(vs, nullptr, 0);
		a_context->PSSetShader(ps, nullptr, 0);
		a_context->PSSetConstantBuffers(0, 1, &cb);
		ID3D11ShaderResourceView* srvs[2] = { depthSrv, a_frame.scene };
		a_context->PSSetShaderResources(0, 2, srvs);
		a_context->Draw(3, 0);
		ID3D11ShaderResourceView* none[2] = {};
		a_context->PSSetShaderResources(0, 2, none);

		static int logged = 0;
		if (logged++ == 0) {
			logger::info("scene light: lighting Mad Max's world with {} Minecraft light(s)", nearList.size());
		}
	}

	void MeasureShade(ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11Texture2D* a_scene, UINT a_width, UINT a_height, DXGI_FORMAT a_format)
	{
		ReadSettings();
		auto& st = State();
		if (!shadeOn || !a_scene) {
			st.shade = 1.0f;
			sceneFactor = 1.0f;
			return;
		}
		// The mip about 32 pixels wide.
		UINT mip = 0;
		while ((a_width >> (mip + 1)) >= 32 && (a_height >> (mip + 1)) >= 8) {
			++mip;
		}
		const UINT w = std::max(1u, a_width >> mip), h = std::max(1u, a_height >> mip);
		if (w != stagingW || h != stagingH || a_format != stagingFormat || !staging[0]) {
			for (int i = 0; i < kStaging; ++i) {
				Release(staging[i]);
				stagingFilled[i] = false;
			}
			D3D11_TEXTURE2D_DESC td{};
			td.Width = w;
			td.Height = h;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = a_format;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_STAGING;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			for (int i = 0; i < kStaging; ++i) {
				if (FAILED(a_device->CreateTexture2D(&td, nullptr, &staging[i]))) {
					static bool warned = false;
					if (!std::exchange(warned, true)) {
						logger::warn("scene light: no read-back texture (format {}); the player keeps full light", static_cast<int>(a_format));
					}
					return;
				}
			}
			stagingW = w, stagingH = h, stagingFormat = a_format, stagingMip = mip;
			logger::info("scene light: measuring the frame around the player from a {}x{} mip (format {})", w, h, static_cast<int>(a_format));
		}
		// Read the oldest one back (two frames old: the GPU is done with it), then refill it.
		const int slot = stagingNext;
		stagingNext = (stagingNext + 1) % kStaging;
		if (stagingFilled[slot]) {
			D3D11_MAPPED_SUBRESOURCE map{};
			if (SUCCEEDED(a_context->Map(staging[slot], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map))) {
				float luma = 0.0f;
				const bool ok = Average(map, luma);
				a_context->Unmap(staging[slot], 0);
				if (ok) {
					LARGE_INTEGER now{}, freq{};
					::QueryPerformanceCounter(&now);
					::QueryPerformanceFrequency(&freq);
					const float dt = lastShadeQpc ? float(double(now.QuadPart - lastShadeQpc) / double(freq.QuadPart)) : 0.0f;
					lastShadeQpc = now.QuadPart;
					// Light like the eye adapts: quick, but no popping as the view swings.
					const float target = std::clamp(luma / shadeRef, 0.0f, 1.0f);
					sceneFactor += (target - sceneFactor) * (1.0f - std::exp(-std::min(dt, 0.25f) / 0.35f));
					// Minecraft's light levels aren't linear: the level whose brightness is this.
					float shade = 4.0f * sceneFactor / (1.0f + 3.0f * sceneFactor);
					shade = std::max(shade, shadeMin);
					if (const auto held = st.heldLight.load(); (held & 0xFF) != 0) {
						shade = std::max(shade, (held & 0xFF) / 15.0f * 0.85f);  // a torch in hand lights the player
					}
					st.shade = shade;
					static std::uint64_t lastLog = 0;
					if (::GetTickCount64() - lastLog > 10000) {
						lastLog = ::GetTickCount64();
						logger::info("scene light: frame brightness {:.3f} -> player light {:.2f} (Minecraft sky level {})", luma, shade, int(std::lround(shade * 15.0f)));
					}
				}
			}
		}
		a_context->CopySubresourceRegion(staging[slot], 0, 0, 0, 0, a_scene, stagingMip, nullptr);
		stagingFilled[slot] = true;
	}

	float SceneFactor()
	{
		return sceneFactor;
	}
}
