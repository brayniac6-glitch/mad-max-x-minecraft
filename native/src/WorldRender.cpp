#include "WorldRender.h"

#include "Game.h"

#include <MinHook.h>
#include <d3dcompiler.h>

// Minecraft's things inside Mad Max's 3D world: placed blocks (with Minecraft's own models, textures,
// tint, AO and light), the player's Steve body, other entities, particles, dropped items, arrows,
// block cracks and the block outline. Minecraft ships the meshes through the render ring (see
// proto::RenType); they're drawn with Mad Max's own camera and depth-tested against Mad Max's depth
// buffer, so they stay locked to the wasteland and hide behind its hills and walls.
//
// Ported from SkyCraft's WorldRender (MIT, (c) 2026 chasmlol): the message handling, meshes, atlas,
// entities and passes are SkyCraft's. The Skyrim parts (lighting from its sun/weather/interiors, its
// shadow maps, its in-frame render hook) are replaced by: Mad Max's camera matrices (MadMax.h), its
// scene depth found through ClearDepthStencilView, and Minecraft's own lighting.
namespace madcraft
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

		using Vertex = proto::RenVertex;
		constexpr std::uint32_t kFlagCutout = 1;
		constexpr std::uint32_t kFlagTranslucent = 2;
		constexpr std::uint32_t kFlagUntextured = 4;
		constexpr std::uint32_t kFlagNoMip = 8;
		constexpr std::uint32_t kFlagNormalFromFaces = 7u << 4;
		constexpr std::uint32_t kFullSkyLight = 15u << 8;
		constexpr float         kPi = 3.14159265f;

		constexpr char kShader[] = R"(
cbuffer Frame : register(b0)
{
	row_major float4x4 viewProj;  // camera-relative Mad Max world (metres, Y up) -> clip, row vectors
	float4 light;                 // x: exposure, y: minimum brightness, z: lighting model on, w: scene tint on
	float4 axes;                  // x, z: +1/-1 Minecraft -> Mad Max axis signs
	float4 sunDir;                // towards the sun (or moon), Mad Max axes; w: night (0..1)
	float4 sunColor;              // rgb
	float4 ambient;               // rgb (the sky's fill light)
	float4 fog;                   // x: start (m), y: end (m), z: max amount, w: on
};
cbuffer Object : register(b1)
{
	float4 offset;                // camera-relative Mad Max position of the mesh's Minecraft origin
};
Texture2D atlas : register(t0);
Texture2D sceneColor : register(t1);  // Mad Max's frame (mipmapped): its average tints our light and haze
SamplerState atlasSampler : register(s0);
SamplerState smoothSampler : register(s1);

struct VSIn
{
	float3 pos : POSITION;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
	uint light : TEXCOORD1;
	uint flags : TEXCOORD2;
};
struct VSOut
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
	float2 light : TEXCOORD1;
	nointerpolation uint flags : TEXCOORD2;
	float3 rel : TEXCOORD3;
};

VSOut VSMain(VSIn i)
{
	VSOut o;
	float3 rel = offset.xyz + float3(i.pos.x * axes.x, i.pos.y, i.pos.z * axes.z);
	o.pos = mul(float4(rel, 1.0), viewProj);
	o.uv = i.uv;
	o.color = i.color;
	o.light = float2(i.light & 0xFF, (i.light >> 8) & 0xFF) / 15.0;
	o.flags = i.flags;
	o.rel = rel;
	return o;
}

float Curve(float l) { return l / (4.0 - 3.0 * l); }  // Minecraft's light-level falloff

// Face normals by Minecraft Direction ordinal + 1 (down, up, north, south, west, east), Minecraft axes.
static const float3 kNormals[8] = {
	float3(0, 0, 0), float3(0, -1, 0), float3(0, 1, 0), float3(0, 0, -1),
	float3(0, 0, 1), float3(-1, 0, 0), float3(1, 0, 0), float3(0, 0, 0) };

// SkyCraft's lighting (MIT, chasmlol), with Mad Max's sun: the sky's fill light (darkened where
// Minecraft's sky light says a face is roofed over), plus the sun on faces turned towards it, through
// a soft exposure curve into the game's range; Minecraft's block light (torches) still glows.
float3 Lighting(float2 l, float3 n, bool hasNormal, float3 tint)
{
	float sky = Curve(l.y);
	float sun = hasNormal ? saturate(dot(n, sunDir.xyz)) : 0.35 + 0.4 * saturate(sunDir.y);
	float3 lit = ambient.rgb * tint * lerp(0.3, 1.0, sky) + sunColor.rgb * tint * (sun * sky * sky);
	lit = 1.0 - exp(-max(lit, 0.0) * light.x);
	float block = Curve(l.x);
	return max(max(lit, light.y), block * float3(1.0, 0.85, 0.65));
}

float4 PSMain(VSOut i) : SV_Target
{
	if (i.flags & 4) {
		return i.color;
	}
	float4 t = (i.flags & 8) ? atlas.SampleLevel(atlasSampler, i.uv, 0) : atlas.Sample(atlasSampler, i.uv);
	if (!(i.flags & 2) && t.a < 0.5) {
		discard;
	}
	uint   ni = (i.flags >> 4) & 7;
	float3 n = kNormals[ni];
	n = float3(n.x * axes.x, n.y, n.z * axes.z);
	if (ni == 7) {
		n = normalize(cross(ddy(i.rel), ddx(i.rel)));  // entity models: the triangle's own normal
		n = dot(n, i.rel) > 0 ? -n : n;
	}
	// The scene's overall colour (one smooth average of Mad Max's frame), so the blocks pick up its
	// grading and the time of day's tone without looking like a pasted-on noon.
	float3 avg = light.w > 0.5 ? sceneColor.SampleLevel(smoothSampler, float2(0.5, 0.5), 14.0).rgb : 0.5;
	float  avgL = max(dot(avg, float3(0.2126, 0.7152, 0.0722)), 0.02);
	float3 tint = light.w > 0.5 ? lerp(1.0, avg / avgL, 0.35) : 1.0;
	float3 lit = light.z > 0.5 ? Lighting(i.light, n, ni != 0, tint) : max(Curve(i.light.y), light.y);
	float3 c = t.rgb * i.color.rgb * lit;
	if (fog.w > 0.5) {
		// Mad Max's dusty haze: towards the scene's own average colour with distance.
		float f = saturate((length(i.rel) - fog.x) / max(fog.y - fog.x, 1.0)) * fog.z;
		c = lerp(c, avg, f);
	}
	return float4(saturate(c), (i.flags & 2) ? t.a * i.color.a : 1.0);
})";

		struct alignas(16) FrameConstants
		{
			float viewProj[4][4];
			float light[4];
			float axes[4];
			float sunDir[4];
			float sunColor[4];
			float ambient[4];
			float fog[4];
		};

		ID3D11SamplerState* smoothSampler = nullptr;

		// Sun, sky and haze for the hour (Mad Max's clock, sheet/hooks.tsv H08). The sun rises in the
		// east, peaks at noon and sets in the west; [Render] fSunAzimuthDeg turns that path to match
		// Mad Max's shadows. Colours follow the desert day: warm white, orange near the horizon, a dim
		// blue moon at night. Like SkyCraft's GatherLighting, from Skyrim's sky and weather.
		void SetDaylight(FrameConstants& a_fc, float a_hour)
		{
			const float azimuth = static_cast<float>(IniDouble("Render", "fSunAzimuthDeg", 0.0)) * kPi / 180.0f;
			const float dayAngle = (a_hour - 6.0f) / 12.0f * kPi;  // 0 at 6:00 (east), pi at 18:00 (west)
			float       elev = std::sin(dayAngle);
			const bool  night = elev < 0.0f;
			const float across = std::cos(dayAngle);
			// The moon takes the sun's place, opposite it, at night.
			const float dx = (night ? -across : across), dy = std::fabs(elev);
			float       d[3] = { dx * std::cos(azimuth), std::max(dy, 0.08f), dx * std::sin(azimuth) + 0.25f };
			const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			const float nightAmount = std::clamp(-elev * 4.0f, 0.0f, 1.0f);
			const float horizon = std::clamp(1.0f - dy * 2.5f, 0.0f, 1.0f);  // low sun: orange
			for (int k = 0; k < 3; ++k) {
				a_fc.sunDir[k] = d[k] / len;
			}
			a_fc.sunDir[3] = nightAmount;
			const float day[3] = { 1.15f, 1.02f, 0.86f }, dusk[3] = { 1.2f, 0.62f, 0.32f }, moon[3] = { 0.16f, 0.2f, 0.32f };
			const float ambDay[3] = { 0.55f, 0.58f, 0.62f }, ambNight[3] = { 0.07f, 0.09f, 0.15f };
			for (int k = 0; k < 3; ++k) {
				const float sun = day[k] + (dusk[k] - day[k]) * horizon;
				a_fc.sunColor[k] = sun + (moon[k] - sun) * nightAmount;
				a_fc.ambient[k] = ambDay[k] + (ambNight[k] - ambDay[k]) * nightAmount;
			}
		}

		// A mipmapped copy of Mad Max's frame (before our blocks): the scene's light for the blocks.
		ID3D11Texture2D*          sceneTex = nullptr;
		ID3D11ShaderResourceView* sceneSrv = nullptr;
		UINT                      sceneW = 0, sceneH = 0;
		DXGI_FORMAT               sceneFormat = DXGI_FORMAT_UNKNOWN;
		bool                      sceneFailed = false;

		bool CopyScene(ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11Texture2D* a_backBuffer, const D3D11_TEXTURE2D_DESC& a_desc)
		{
			if (sceneFailed) {
				return false;
			}
			if (!sceneTex || sceneW != a_desc.Width || sceneH != a_desc.Height || sceneFormat != a_desc.Format) {
				if (sceneSrv) {
					sceneSrv->Release();
					sceneSrv = nullptr;
				}
				if (sceneTex) {
					sceneTex->Release();
					sceneTex = nullptr;
				}
				D3D11_TEXTURE2D_DESC td{};
				td.Width = a_desc.Width;
				td.Height = a_desc.Height;
				td.MipLevels = 0;  // full chain, down to 1x1
				td.ArraySize = 1;
				td.Format = a_desc.Format;
				td.SampleDesc.Count = 1;
				td.Usage = D3D11_USAGE_DEFAULT;
				td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
				td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
				if (FAILED(a_device->CreateTexture2D(&td, nullptr, &sceneTex)) || FAILED(a_device->CreateShaderResourceView(sceneTex, nullptr, &sceneSrv))) {
					logger::warn("world renderer: no scene copy (format {}); blocks keep Minecraft's own light", static_cast<int>(a_desc.Format));
					sceneFailed = true;
					return false;
				}
				sceneW = a_desc.Width, sceneH = a_desc.Height, sceneFormat = a_desc.Format;
			}
			// Top mip only, from the back buffer; the rest is generated (a blur pyramid).
			a_context->CopySubresourceRegion(sceneTex, 0, 0, 0, 0, a_backBuffer, 0, nullptr);
			a_context->GenerateMips(sceneSrv);
			return true;
		}

		struct alignas(16) ObjectConstants
		{
			float offset[4];
		};

		struct Section
		{
			ID3D11Buffer* vb{ nullptr };
			std::uint32_t opaque{ 0 };
			std::uint32_t translucent{ 0 };
			std::int32_t  sx{ 0 }, sy{ 0 }, sz{ 0 };
		};

		struct EntityTexture
		{
			ID3D11Texture2D*          tex{ nullptr };
			ID3D11ShaderResourceView* srv{ nullptr };
		};

		struct Mesh
		{
			std::vector<proto::RenBatch> batches;
			ID3D11Buffer*                vb{ nullptr };
			UINT                         capacity{ 0 };
			double                       origin[3]{};
		};

		ID3D11Device*            device = nullptr;
		bool                     initFailed = false;
		ID3D11VertexShader*      vs = nullptr;
		ID3D11PixelShader*       ps = nullptr;
		ID3D11InputLayout*       layout = nullptr;
		ID3D11Buffer*            frameCb = nullptr;
		ID3D11Buffer*            objectCb = nullptr;
		ID3D11Buffer*            dynVb = nullptr;
		UINT                     dynCapacity = 0;
		ID3D11SamplerState*      atlasSampler = nullptr;
		ID3D11RasterizerState*   raster = nullptr;
		ID3D11RasterizerState*   rasterCullBack = nullptr;
		ID3D11DepthStencilState* depthWrite = nullptr;
		ID3D11DepthStencilState* depthRead = nullptr;
		ID3D11DepthStencilState* depthWriteRev = nullptr;
		ID3D11DepthStencilState* depthReadRev = nullptr;
		ID3D11BlendState*        noBlend = nullptr;
		ID3D11BlendState*        alphaBlend = nullptr;
		ID3D11Texture2D*         ownDepth = nullptr;
		ID3D11DepthStencilView*  ownDsv = nullptr;
		UINT                     ownDepthW = 0, ownDepthH = 0;
		ID3D11Texture2D*         atlasTex = nullptr;
		ID3D11ShaderResourceView* atlasSrv = nullptr;
		bool                     atlasMipsStale = false;

		std::unordered_map<std::uint64_t, Section>       sections;
		std::unordered_map<std::uint32_t, EntityTexture> entityTextures;
		Mesh                                             avatar;  // the player's Steve body, relative to the feet
		Mesh                                             scene;   // every other entity and all particles
		std::vector<Vertex>                              scratch;
		std::vector<Vertex>                              dynVerts;
		proto::WorldEntities                             entities{};

		// ---- Mad Max's scene depth ------------------------------------------------------------
		// Mad Max clears its main depth buffer at the start of each frame. Every screen-sized depth
		// buffer it clears is noted (with its clear value, which tells standard from reversed Z); at
		// Present the first one of the frame is the scene's, still holding the finished frame's depth.
		using ClearDsvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
		ClearDsvFn origClearDsv = nullptr;
		std::mutex depthLock;
		struct DepthCandidate
		{
			ID3D11DepthStencilView* dsv{ nullptr };
			float                   clear{ 1.0f };
		};
		std::vector<DepthCandidate> frameDepths;
		UINT                        screenW = 0, screenH = 0;

		void STDMETHODCALLTYPE HookClearDsv(ID3D11DeviceContext* a_ctx, ID3D11DepthStencilView* a_dsv, UINT a_flags, FLOAT a_depth, UINT8 a_stencil)
		{
			if (a_dsv && (a_flags & D3D11_CLEAR_DEPTH) && screenW) {
				ID3D11Resource* res = nullptr;
				a_dsv->GetResource(&res);
				ID3D11Texture2D* tex = nullptr;
				if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
					D3D11_TEXTURE2D_DESC td{};
					tex->GetDesc(&td);
					if (td.Width == screenW && td.Height == screenH && td.SampleDesc.Count == 1 && a_dsv != ownDsv) {
						std::lock_guard g{ depthLock };
						if (frameDepths.size() < 8) {
							a_dsv->AddRef();
							frameDepths.push_back({ a_dsv, a_depth });
						}
					}
					tex->Release();
				}
				Release(res);
			}
			origClearDsv(a_ctx, a_dsv, a_flags, a_depth, a_stencil);
		}

		void HookDepthClears(ID3D11DeviceContext* a_context)
		{
			if (origClearDsv) {
				return;
			}
			void* target = (*reinterpret_cast<void***>(a_context))[53];  // ID3D11DeviceContext::ClearDepthStencilView
			if (MH_CreateHook(target, reinterpret_cast<void*>(&HookClearDsv), reinterpret_cast<void**>(&origClearDsv)) == MH_OK && MH_EnableHook(target) == MH_OK) {
				logger::info("world renderer: watching depth clears for Mad Max's scene depth");
			}
		}

		// ---- setup ------------------------------------------------------------------------------
		bool Compile(const char* a_entry, const char* a_target, ID3DBlob** a_out)
		{
			ID3DBlob*  errors = nullptr;
			const auto hr = D3DCompile(kShader, sizeof(kShader) - 1, "madcraft_world", nullptr, nullptr, a_entry, a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, a_out, &errors);
			if (FAILED(hr)) {
				logger::error("world shader {} failed: {}", a_entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
				Release(errors);
				return false;
			}
			Release(errors);
			return true;
		}

		bool Init(ID3D11Device* a_device)
		{
			if (device) {
				return true;
			}
			if (initFailed) {
				return false;
			}
			ID3DBlob *vsBlob = nullptr, *psBlob = nullptr;
			if (!Compile("VSMain", "vs_5_0", &vsBlob) || !Compile("PSMain", "ps_5_0", &psBlob)) {
				initFailed = true;
				return false;
			}
			a_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
			a_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
			const D3D11_INPUT_ELEMENT_DESC elements[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 2, DXGI_FORMAT_R32_UINT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			};
			a_device->CreateInputLayout(elements, 5, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout);
			Release(vsBlob);
			Release(psBlob);

			D3D11_BUFFER_DESC cbd{};
			cbd.Usage = D3D11_USAGE_DYNAMIC;
			cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			cbd.ByteWidth = sizeof(FrameConstants);
			a_device->CreateBuffer(&cbd, nullptr, &frameCb);
			cbd.ByteWidth = sizeof(ObjectConstants);
			a_device->CreateBuffer(&cbd, nullptr, &objectCb);

			D3D11_SAMPLER_DESC sd{};
			sd.Filter = D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR;  // crisp pixels like Minecraft, mipmapped at range
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.MaxLOD = D3D11_FLOAT32_MAX;
			a_device->CreateSamplerState(&sd, &atlasSampler);
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;  // the scene average: smooth, never blocky
			a_device->CreateSamplerState(&sd, &smoothSampler);

			D3D11_RASTERIZER_DESC rd{};
			rd.FillMode = D3D11_FILL_SOLID;
			rd.CullMode = D3D11_CULL_NONE;
			rd.DepthClipEnable = TRUE;
			a_device->CreateRasterizerState(&rd, &raster);
			rd.CullMode = D3D11_CULL_BACK;
			a_device->CreateRasterizerState(&rd, &rasterCullBack);

			D3D11_DEPTH_STENCIL_DESC dd{};
			dd.DepthEnable = TRUE;
			dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
			a_device->CreateDepthStencilState(&dd, &depthWrite);
			dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			a_device->CreateDepthStencilState(&dd, &depthRead);
			dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
			a_device->CreateDepthStencilState(&dd, &depthReadRev);
			dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			a_device->CreateDepthStencilState(&dd, &depthWriteRev);

			D3D11_BLEND_DESC bd{};
			bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
			a_device->CreateBlendState(&bd, &noBlend);
			bd.RenderTarget[0].BlendEnable = TRUE;
			bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
			bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
			bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			a_device->CreateBlendState(&bd, &alphaBlend);

			const bool ok = vs && ps && layout && frameCb && objectCb && atlasSampler && raster && rasterCullBack && depthWrite && depthRead && depthWriteRev &&
			                depthReadRev && noBlend && alphaBlend;
			if (!ok) {
				logger::error("world renderer failed to initialize");
				initFailed = true;
				return false;
			}
			device = a_device;
			logger::info("world renderer ready");
			return true;
		}

		// ---- messages from Minecraft (SkyCraft's, unchanged in substance) -----------------------
		std::uint64_t Key(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z)
		{
			return (std::uint64_t(std::uint32_t(a_x) & 0x1FFFFF) << 42) | (std::uint64_t(std::uint32_t(a_y) & 0x1FFFFF) << 21) | (std::uint32_t(a_z) & 0x1FFFFF);
		}

		void ClearSections()
		{
			for (auto& [key, s] : sections) {
				Release(s.vb);
			}
			sections.clear();
		}

		void ClearAvatar()
		{
			for (auto& [id, t] : entityTextures) {
				Release(t.srv);
				Release(t.tex);
			}
			entityTextures.clear();
			avatar.batches.clear();
			scene.batches.clear();
		}

		void OnTexture(const std::uint8_t* a_data, std::uint32_t a_bytes)
		{
			if (a_bytes < sizeof(proto::RenTexture)) {
				return;
			}
			const auto* hdr = reinterpret_cast<const proto::RenTexture*>(a_data);
			if (!hdr->width || !hdr->height || hdr->width > 4096 || hdr->height > 4096 ||
				a_bytes < sizeof(proto::RenTexture) + std::uint64_t(hdr->width) * hdr->height * 4) {
				return;
			}
			auto& t = entityTextures[hdr->id];
			Release(t.srv);
			Release(t.tex);
			D3D11_TEXTURE2D_DESC td{};
			td.Width = hdr->width;
			td.Height = hdr->height;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_IMMUTABLE;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA init{ a_data + sizeof(proto::RenTexture), hdr->width * 4, 0 };
			if (FAILED(device->CreateTexture2D(&td, &init, &t.tex)) || FAILED(device->CreateShaderResourceView(t.tex, nullptr, &t.srv))) {
				Release(t.tex);
				entityTextures.erase(hdr->id);
				return;
			}
			logger::info("received Minecraft entity texture {} ({}x{})", hdr->id, hdr->width, hdr->height);
		}

		void OnMesh(ID3D11DeviceContext* a_context, Mesh& a_mesh, const std::uint8_t* a_data, std::uint32_t a_bytes, bool a_hasOrigin)
		{
			a_mesh.batches.clear();
			const std::size_t head = (a_hasOrigin ? sizeof(proto::RenScene) : sizeof(proto::RenAvatar));
			if (a_bytes < head) {
				return;
			}
			std::uint32_t batchCount, vertexCount;
			if (a_hasOrigin) {
				const auto* hdr = reinterpret_cast<const proto::RenScene*>(a_data);
				a_mesh.origin[0] = hdr->originX;
				a_mesh.origin[1] = hdr->originY;
				a_mesh.origin[2] = hdr->originZ;
				batchCount = hdr->batchCount;
				vertexCount = hdr->vertexCount;
			} else {
				const auto* hdr = reinterpret_cast<const proto::RenAvatar*>(a_data);
				batchCount = hdr->batchCount;
				vertexCount = hdr->vertexCount;
			}
			const std::uint64_t need = head + std::uint64_t(batchCount) * sizeof(proto::RenBatch) + std::uint64_t(vertexCount) * sizeof(Vertex);
			if (batchCount == 0 || vertexCount == 0 || a_bytes < need) {
				return;
			}
			const auto* batches = reinterpret_cast<const proto::RenBatch*>(a_data + head);
			const auto* verts = reinterpret_cast<const Vertex*>(batches + batchCount);
			const UINT  bytes = vertexCount * sizeof(Vertex);
			if (bytes > a_mesh.capacity) {
				Release(a_mesh.vb);
				D3D11_BUFFER_DESC bd{};
				bd.ByteWidth = std::max<UINT>(bytes * 2, 256 * 1024);
				bd.Usage = D3D11_USAGE_DYNAMIC;
				bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
				bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				if (FAILED(device->CreateBuffer(&bd, nullptr, &a_mesh.vb))) {
					a_mesh.capacity = 0;
					return;
				}
				a_mesh.capacity = bd.ByteWidth;
			}
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(a_context->Map(a_mesh.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				return;
			}
			std::memcpy(mapped.pData, verts, bytes);
			a_context->Unmap(a_mesh.vb, 0);
			for (std::uint32_t b = 0; b < batchCount; ++b) {
				if (batches[b].first + batches[b].count <= vertexCount) {
					a_mesh.batches.push_back(batches[b]);
				}
			}
			static bool loggedAvatar = false, loggedScene = false;
			if (!(a_hasOrigin ? loggedScene : loggedAvatar)) {
				(a_hasOrigin ? loggedScene : loggedAvatar) = true;
				logger::info("{}: {} triangles in {} texture batches", a_hasOrigin ? "Minecraft entities and particles" : "Steve's body", vertexCount / 3, batchCount);
			}
		}

		void OnAtlasRegion(ID3D11DeviceContext* a_context, const std::uint8_t* a_data, std::uint32_t a_bytes)
		{
			if (!atlasTex || a_bytes < sizeof(proto::RenAtlasRegion)) {
				return;
			}
			const auto* hdr = reinterpret_cast<const proto::RenAtlasRegion*>(a_data);
			D3D11_TEXTURE2D_DESC td{};
			atlasTex->GetDesc(&td);
			if (!hdr->width || !hdr->height || hdr->x + hdr->width > td.Width || hdr->y + hdr->height > td.Height ||
				a_bytes < sizeof(proto::RenAtlasRegion) + std::uint64_t(hdr->width) * hdr->height * 4) {
				return;
			}
			const D3D11_BOX box{ hdr->x, hdr->y, 0, hdr->x + hdr->width, hdr->y + hdr->height, 1 };
			a_context->UpdateSubresource(atlasTex, 0, &box, a_data + sizeof(proto::RenAtlasRegion), hdr->width * 4, 0);
			atlasMipsStale = true;
		}

		void OnAtlas(ID3D11DeviceContext* a_context, const std::uint8_t* a_data, std::uint32_t a_bytes)
		{
			if (a_bytes < sizeof(proto::RenAtlas)) {
				return;
			}
			const auto* hdr = reinterpret_cast<const proto::RenAtlas*>(a_data);
			if (a_bytes < sizeof(proto::RenAtlas) + std::uint64_t(hdr->width) * hdr->height * 4 || !hdr->width || !hdr->height) {
				return;
			}
			Release(atlasSrv);
			Release(atlasTex);
			D3D11_TEXTURE2D_DESC td{};
			td.Width = hdr->width;
			td.Height = hdr->height;
			td.MipLevels = 5;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
			if (FAILED(device->CreateTexture2D(&td, nullptr, &atlasTex)) || FAILED(device->CreateShaderResourceView(atlasTex, nullptr, &atlasSrv))) {
				logger::error("atlas texture {}x{} failed", hdr->width, hdr->height);
				Release(atlasTex);
				return;
			}
			a_context->UpdateSubresource(atlasTex, 0, nullptr, a_data + sizeof(proto::RenAtlas), hdr->width * 4, 0);
			a_context->GenerateMips(atlasSrv);
			logger::info("received Minecraft texture atlas {}x{}", hdr->width, hdr->height);
		}

		void OnSection(const std::uint8_t* a_data, std::uint32_t a_bytes)
		{
			if (a_bytes < sizeof(proto::RenSection)) {
				return;
			}
			const auto* hdr = reinterpret_cast<const proto::RenSection*>(a_data);
			const auto  key = Key(hdr->sx, hdr->sy, hdr->sz);
			if (auto it = sections.find(key); it != sections.end()) {
				Release(it->second.vb);
				sections.erase(it);
			}
			const std::uint32_t count = hdr->vertexCount - hdr->vertexCount % 3;
			if (count == 0 || a_bytes < sizeof(proto::RenSection) + std::uint64_t(count) * sizeof(Vertex)) {
				return;
			}
			const auto* src = reinterpret_cast<const Vertex*>(a_data + sizeof(proto::RenSection));
			scratch.clear();
			scratch.reserve(count);
			for (int pass = 0; pass < 2; ++pass) {
				for (std::uint32_t t = 0; t < count; t += 3) {
					const bool translucent = (src[t].flags & kFlagTranslucent) != 0;
					if (translucent == (pass == 1)) {
						scratch.insert(scratch.end(), src + t, src + t + 3);
					}
				}
				if (pass == 0) {
					sections[key].opaque = static_cast<std::uint32_t>(scratch.size());
				}
			}
			Section& s = sections[key];
			s.translucent = static_cast<std::uint32_t>(scratch.size()) - s.opaque;
			s.sx = hdr->sx;
			s.sy = hdr->sy;
			s.sz = hdr->sz;
			D3D11_BUFFER_DESC bd{};
			bd.ByteWidth = static_cast<UINT>(scratch.size() * sizeof(Vertex));
			bd.Usage = D3D11_USAGE_IMMUTABLE;
			bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
			D3D11_SUBRESOURCE_DATA init{ scratch.data(), 0, 0 };
			if (FAILED(device->CreateBuffer(&bd, &init, &s.vb))) {
				sections.erase(key);
			}
			static int logged = 0;
			if (logged++ < 3) {
				logger::info("received Minecraft block section ({}, {}, {}): {} triangles", hdr->sx, hdr->sy, hdr->sz, count / 3);
			}
		}

		void DrainMessages(ID3D11DeviceContext* a_context)
		{
			Link::Get().DrainRender(
				[&](std::uint32_t a_type, const std::uint8_t* a_data, std::uint32_t a_bytes) {
					switch (a_type) {
					case proto::kRenAtlas:
						OnAtlas(a_context, a_data, a_bytes);
						break;
					case proto::kRenSection:
						OnSection(a_data, a_bytes);
						break;
					case proto::kRenClearAll:
						ClearSections();
						ClearAvatar();
						break;
					case proto::kRenTexture:
						OnTexture(a_data, a_bytes);
						break;
					case proto::kRenAvatar:
						OnMesh(a_context, avatar, a_data, a_bytes, false);
						break;
					case proto::kRenScene:
						OnMesh(a_context, scene, a_data, a_bytes, true);
						break;
					case proto::kRenAtlasRegion:
						OnAtlasRegion(a_context, a_data, a_bytes);
						break;
					default:  // lights, solids, ragdoll: not yet on the Mad Max side
						break;
					}
				},
				48ull << 20);
			if (std::exchange(atlasMipsStale, false) && atlasSrv) {
				a_context->GenerateMips(atlasSrv);
			}
		}

		// ---- per-frame geometry for entities and the outline (SkyCraft's) ----------------------
		void Quad(const float a_p[4][3], const float a_uv[4], std::uint32_t a_color = 0xFFFFFFFFu, std::uint32_t a_flags = kFlagCutout)
		{
			const float uv[4][2] = { { a_uv[0], a_uv[1] }, { a_uv[2], a_uv[1] }, { a_uv[2], a_uv[3] }, { a_uv[0], a_uv[3] } };
			for (int k : { 0, 1, 2, 0, 2, 3 }) {
				dynVerts.push_back({ a_p[k][0], a_p[k][1], a_p[k][2], uv[k][0], uv[k][1], a_color, kFullSkyLight, a_flags });
			}
		}

		std::uint32_t Shade(float a_shade, std::uint32_t a_tint = 0)
		{
			float r = a_shade, g = a_shade, b = a_shade;
			if (a_tint) {
				r *= float(a_tint & 0xFF) / 255.0f;
				g *= float((a_tint >> 8) & 0xFF) / 255.0f;
				b *= float((a_tint >> 16) & 0xFF) / 255.0f;
			}
			return 0xFF000000u | (std::uint32_t(b * 255.0f) << 16) | (std::uint32_t(g * 255.0f) << 8) | std::uint32_t(r * 255.0f);
		}

		void Box(const float a_min[3], const float a_size[3], float a_yaw, const float a_side[4], const float a_top[4], const float a_bottom[4],
			std::uint32_t a_topTint, std::uint32_t a_flags, bool a_shaded)
		{
			const float cx = a_min[0] + a_size[0] * 0.5f, cz = a_min[2] + a_size[2] * 0.5f;
			const float c = std::cos(a_yaw), s = std::sin(a_yaw);
			auto        corner = [&](int a_i, float a_out[3]) {
				const float lx = ((a_i & 1) ? 0.5f : -0.5f) * a_size[0], lz = ((a_i & 4) ? 0.5f : -0.5f) * a_size[2];
				a_out[0] = cx + lx * c - lz * s;
				a_out[1] = a_min[1] + ((a_i & 2) ? a_size[1] : 0.0f);
				a_out[2] = cz + lx * s + lz * c;
			};
			static constexpr int kFaces[6][4] = {
				{ 6, 7, 5, 4 }, { 3, 2, 0, 1 }, { 7, 3, 1, 5 }, { 2, 6, 4, 0 }, { 2, 3, 7, 6 }, { 4, 5, 1, 0 },
			};
			for (int f = 0; f < 6; ++f) {
				float p[4][3];
				for (int k = 0; k < 4; ++k) {
					corner(kFaces[f][k], p[k]);
				}
				const float*        uv = f == 4 ? a_top : f == 5 ? a_bottom : a_side;
				const std::uint32_t color = a_shaded ? Shade(1.0f, f == 4 ? a_topTint : 0) : 0xFFFFFFFFu;
				Quad(p, uv, color, a_flags | kFlagNormalFromFaces);
			}
		}

		constexpr float kArrowScale = 0.55f;

		void BuildArrow(float px, float py, float pz, const float d[3], const float* a_uvSide, const float* a_uvBack, bool a_trident)
		{
			float s[3] = { d[2], 0.0f, -d[0] };
			float sl = std::sqrt(s[0] * s[0] + s[2] * s[2]);
			if (sl < 1e-3f) {
				s[0] = 1.0f, s[2] = 0.0f, sl = 1.0f;
			}
			s[0] /= sl, s[2] /= sl;
			const float     u[3] = { s[1] * d[2] - s[2] * d[1], s[2] * d[0] - s[0] * d[2], s[0] * d[1] - s[1] * d[0] };
			constexpr float r = 0.70710678f;
			const float     fins[2][3] = { { (u[0] + s[0]) * r, (u[1] + s[1]) * r, (u[2] + s[2]) * r }, { (u[0] - s[0]) * r, (u[1] - s[1]) * r, (u[2] - s[2]) * r } };
			auto            at = [&](float a_along, const float* a_q, float a_side, const float* a_q2, float a_side2, float a_out[3]) {
				for (int k = 0; k < 3; ++k) {
					a_out[k] = (k == 0 ? px : k == 1 ? py : pz) + d[k] * a_along + a_q[k] * a_side + (a_q2 ? a_q2[k] * a_side2 : 0.0f);
				}
			};
			if (!a_trident) {
				constexpr float k = 0.9f / 16.0f * kArrowScale;
				for (const auto& q : fins) {
					float p[4][3];
					at(-12 * k, q, -2 * k, nullptr, 0, p[0]);
					at(4 * k, q, -2 * k, nullptr, 0, p[1]);
					at(4 * k, q, 2 * k, nullptr, 0, p[2]);
					at(-12 * k, q, 2 * k, nullptr, 0, p[3]);
					Quad(p, a_uvSide, 0xFFFFFFFFu, kFlagCutout | kFlagNoMip);
				}
				float p[4][3];
				at(-11 * k, fins[0], -2 * k, fins[1], -2 * k, p[0]);
				at(-11 * k, fins[0], 2 * k, fins[1], -2 * k, p[1]);
				at(-11 * k, fins[0], 2 * k, fins[1], 2 * k, p[2]);
				at(-11 * k, fins[0], -2 * k, fins[1], 2 * k, p[3]);
				Quad(p, a_uvBack, 0xFFFFFFFFu, kFlagCutout | kFlagNoMip);
			} else {
				constexpr float h = 0.9f;
				for (const auto& q : fins) {
					float p[4][3];
					at(0, q, h, nullptr, 0, p[0]);
					at(h, q, 0, nullptr, 0, p[1]);
					at(0, q, -h, nullptr, 0, p[2]);
					at(-h, q, 0, nullptr, 0, p[3]);
					Quad(p, a_uvSide, 0xFFFFFFFFu, kFlagCutout | kFlagNoMip);
				}
			}
		}

		void BuildEntities(const double a_o[3], std::vector<Vertex>& a_cracks)
		{
			for (std::uint32_t i = 0; i < entities.count; ++i) {
				const auto& e = entities.entities[i];
				const float px = float(e.x - a_o[0]), py = float(e.y - a_o[1]), pz = float(e.z - a_o[2]);
				if (e.kind == proto::kWeShadow) {
					continue;
				}
				if (e.kind == proto::kWeBlock) {
					const float s = e.scale;
					const float mn[3] = { px - s * 0.5f, py - s * 0.5f, pz - s * 0.5f };
					const float sz[3] = { s, s, s };
					Box(mn, sz, e.yaw * kPi / 180.0f, e.uv[0], e.uv[1], e.uv[2], e.tint, kFlagCutout | kFlagNoMip, true);
					continue;
				}
				if (e.kind == proto::kWeCrack) {
					const float mn[3] = { px, py, pz };
					const auto  before = dynVerts.size();
					Box(mn, e.ext, 0.0f, e.uv[0], e.uv[0], e.uv[0], 0, kFlagTranslucent | kFlagNoMip, false);
					a_cracks.insert(a_cracks.end(), dynVerts.begin() + static_cast<std::ptrdiff_t>(before), dynVerts.end());
					dynVerts.resize(before);
					continue;
				}
				if (e.kind == proto::kWeArrow || e.kind == proto::kWeTrident) {
					const float yaw = e.yaw * kPi / 180.0f, pitch = e.pitch * kPi / 180.0f;
					const float d[3] = { std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
					BuildArrow(px, py, pz, d, e.uv[0], e.uv[1], e.kind == proto::kWeTrident);
				} else if (e.kind == proto::kWeItem) {
					const float spin = e.yaw * kPi / 180.0f, half = e.scale * 0.5f;
					const float rx = std::cos(spin) * half, rz = std::sin(spin) * half;
					const float p[4][3] = {
						{ px - rx, py + half, pz - rz },
						{ px + rx, py + half, pz + rz },
						{ px + rx, py - half, pz + rz },
						{ px - rx, py - half, pz - rz },
					};
					Quad(p, e.uv[0], 0xFFFFFFFFu, kFlagCutout | kFlagNoMip);
				}
			}
		}

		void BuildOutline(const double a_o[3])
		{
			if (!entities.hasSelection) {
				return;
			}
			constexpr float g = 0.002f;
			const float     lo[3] = { float(entities.selMin[0] - a_o[0]) - g, float(entities.selMin[1] - a_o[1]) - g, float(entities.selMin[2] - a_o[2]) - g };
			const float     hi[3] = { float(entities.selMax[0] - a_o[0]) + g, float(entities.selMax[1] - a_o[1]) + g, float(entities.selMax[2] - a_o[2]) + g };
			auto            corner = [&](int a_i) {
				return std::array<float, 3>{ (a_i & 1) ? hi[0] : lo[0], (a_i & 2) ? hi[1] : lo[1], (a_i & 4) ? hi[2] : lo[2] };
			};
			static constexpr int    kEdges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
			constexpr std::uint32_t kOutline = 0x73000000u;
			for (const auto& edge : kEdges) {
				for (int k : edge) {
					const auto c = corner(k);
					dynVerts.push_back({ c[0], c[1], c[2], 0, 0, kOutline, kFullSkyLight, kFlagUntextured });
				}
			}
		}

		bool UploadDynamic(ID3D11DeviceContext* a_context)
		{
			if (dynVerts.empty()) {
				return false;
			}
			const UINT bytes = static_cast<UINT>(dynVerts.size() * sizeof(Vertex));
			if (bytes > dynCapacity) {
				Release(dynVb);
				D3D11_BUFFER_DESC bd{};
				bd.ByteWidth = std::max<UINT>(bytes * 2, 64 * 1024);
				bd.Usage = D3D11_USAGE_DYNAMIC;
				bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
				bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				if (FAILED(device->CreateBuffer(&bd, nullptr, &dynVb))) {
					dynCapacity = 0;
					return false;
				}
				dynCapacity = bd.ByteWidth;
			}
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(a_context->Map(dynVb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				return false;
			}
			std::memcpy(mapped.pData, dynVerts.data(), bytes);
			a_context->Unmap(dynVb, 0);
			return true;
		}

		// ---- drawing -----------------------------------------------------------------------------
		Vec3 camPos{};  // this frame's camera, Mad Max world

		// [Render]: SkyCraft-style lighting with Mad Max's sun (bLighting, fExposure), tinted by the
		// scene's overall colour (bSceneTint), and Mad Max's haze (fFogStart/End/Max; 0 = off).
		const bool  lightingModel = IniBool("Render", "bLighting", true);
		const bool  sceneLighting = IniBool("Render", "bSceneTint", true);
		const float exposure = static_cast<float>(IniDouble("Render", "fExposure", 1.6));
		const float fogStart = static_cast<float>(IniDouble("Render", "fFogStart", 60.0));
		const float fogEnd = static_cast<float>(IniDouble("Render", "fFogEnd", 600.0));
		const float fogMax = static_cast<float>(IniDouble("Render", "fFogMax", 0.6));

		// A Minecraft point (blocks) -> camera-relative Mad Max position, in double precision.
		void SetObjectOffset(ID3D11DeviceContext* a_context, const double a_mc[3])
		{
			const Vec3 world = MadMax::FromMc(a_mc[0], a_mc[1], a_mc[2]);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(a_context->Map(objectCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				auto* o = static_cast<ObjectConstants*>(mapped.pData);
				o->offset[0] = float(double(world.x) - camPos.x);
				o->offset[1] = float(double(world.y) - camPos.y);
				o->offset[2] = float(double(world.z) - camPos.z);
				o->offset[3] = 0.0f;
				a_context->Unmap(objectCb, 0);
			}
		}

		void DrawMesh(ID3D11DeviceContext* a_context, const Mesh& a_mesh, const double a_mcOrigin[3], bool a_blended)
		{
			if (a_mesh.batches.empty() || !a_mesh.vb) {
				return;
			}
			SetObjectOffset(a_context, a_mcOrigin);
			const UINT stride = sizeof(Vertex), zero = 0;
			a_context->IASetVertexBuffers(0, 1, &a_mesh.vb, &stride, &zero);
			for (const auto& b : a_mesh.batches) {
				if (((b.flags & 1) != 0) != a_blended) {
					continue;
				}
				ID3D11ShaderResourceView* srv = atlasSrv;
				if (b.texture != 0) {
					const auto it = entityTextures.find(b.texture);
					if (it == entityTextures.end()) {
						continue;
					}
					srv = it->second.srv;
				}
				a_context->PSSetShaderResources(0, 1, &srv);
				a_context->Draw(b.count, b.first);
			}
			a_context->PSSetShaderResources(0, 1, &atlasSrv);
		}

		void DrawEntities(ID3D11DeviceContext* a_context, bool a_blended)
		{
			const auto& st = State();
			if (st.bodyValid) {
				const double feet[3] = { st.bodyX, st.bodyY, st.bodyZ };
				DrawMesh(a_context, avatar, feet, a_blended);
			}
			DrawMesh(a_context, scene, scene.origin, a_blended);
		}

		bool EnsureOwnDepth(UINT a_w, UINT a_h)
		{
			if (ownDsv && ownDepthW == a_w && ownDepthH == a_h) {
				return true;
			}
			Release(ownDsv);
			Release(ownDepth);
			D3D11_TEXTURE2D_DESC td{};
			td.Width = a_w;
			td.Height = a_h;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_D32_FLOAT;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			if (FAILED(device->CreateTexture2D(&td, nullptr, &ownDepth)) || FAILED(device->CreateDepthStencilView(ownDepth, nullptr, &ownDsv))) {
				Release(ownDepth);
				return false;
			}
			ownDepthW = a_w;
			ownDepthH = a_h;
			return true;
		}

		// Everything drawing touches, saved and put back so Mad Max's renderer never notices (SkyCraft's).
		struct StateBackup
		{
			ID3D11RenderTargetView*   rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView*   dsv{};
			ID3D11BlendState*         blend{};
			float                     factor[4]{};
			UINT                      mask{};
			ID3D11RasterizerState*    rasterState{};
			ID3D11DepthStencilState*  depth{};
			UINT                      stencil{};
			D3D11_VIEWPORT            vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT                      vpCount{ D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE };
			D3D11_PRIMITIVE_TOPOLOGY  topo{};
			ID3D11InputLayout*        inputLayout{};
			ID3D11Buffer*             vb{};
			UINT                      stride{}, offset{};
			ID3D11VertexShader*       vsh{};
			ID3D11PixelShader*        psh{};
			ID3D11Buffer*             vsCb[2]{};
			ID3D11Buffer*             psCb[2]{};
			ID3D11ShaderResourceView* srv[2]{};
			ID3D11SamplerState*       samplers[2]{};

			void Save(ID3D11DeviceContext* a_c)
			{
				a_c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
				a_c->OMGetBlendState(&blend, factor, &mask);
				a_c->RSGetState(&rasterState);
				a_c->OMGetDepthStencilState(&depth, &stencil);
				a_c->RSGetViewports(&vpCount, vps);
				a_c->IAGetPrimitiveTopology(&topo);
				a_c->IAGetInputLayout(&inputLayout);
				a_c->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
				a_c->VSGetShader(&vsh, nullptr, nullptr);
				a_c->PSGetShader(&psh, nullptr, nullptr);
				a_c->VSGetConstantBuffers(0, 2, vsCb);
				a_c->PSGetConstantBuffers(0, 2, psCb);
				a_c->PSGetShaderResources(0, 2, srv);
				a_c->PSGetSamplers(0, 2, samplers);
			}

			void Restore(ID3D11DeviceContext* a_c)
			{
				a_c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
				a_c->OMSetBlendState(blend, factor, mask);
				a_c->RSSetState(rasterState);
				a_c->OMSetDepthStencilState(depth, stencil);
				a_c->RSSetViewports(vpCount, vps);
				a_c->IASetPrimitiveTopology(topo);
				a_c->IASetInputLayout(inputLayout);
				a_c->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
				a_c->VSSetShader(vsh, nullptr, 0);
				a_c->PSSetShader(psh, nullptr, 0);
				a_c->VSSetConstantBuffers(0, 2, vsCb);
				a_c->PSSetConstantBuffers(0, 2, psCb);
				a_c->PSSetShaderResources(0, 2, srv);
				a_c->PSSetSamplers(0, 2, samplers);
				for (auto*& r : rtv) {
					Release(r);
				}
				Release(dsv);
				Release(blend);
				Release(rasterState);
				Release(depth);
				Release(inputLayout);
				Release(vb);
				Release(vsh);
				Release(psh);
				for (auto*& b : vsCb) {
					Release(b);
				}
				for (auto*& b : psCb) {
					Release(b);
				}
				Release(srv[0]);
				Release(srv[1]);
				Release(samplers[0]);
				Release(samplers[1]);
			}
		};

		void Render(ID3D11DeviceContext* a_context, IDXGISwapChain* a_swapChain)
		{
			float cam[16], vp[16];
			if (!MadMax::GetCameraMatrix(cam) || !MadMax::GetCameraViewProj(vp)) {
				return;
			}
			camPos = { cam[12], cam[13], cam[14] };

			// Mad Max's world -> clip (row vectors), re-based on the camera so the GPU only sees small
			// camera-relative numbers: row 3 += camPos * rows 0..2.
			FrameConstants fc{};
			for (int r = 0; r < 4; ++r) {
				for (int c = 0; c < 4; ++c) {
					fc.viewProj[r][c] = vp[r * 4 + c];
				}
			}
			for (int c = 0; c < 4; ++c) {
				fc.viewProj[3][c] = float(double(vp[12 + c]) + double(camPos.x) * vp[c] + double(camPos.y) * vp[4 + c] + double(camPos.z) * vp[8 + c]);
			}
			fc.light[0] = exposure;  // SkyCraft's soft exposure curve
			fc.light[1] = 0.04f;     // never fully black
			fc.light[2] = lightingModel ? 1.0f : 0.0f;
			float hour = 12.0f;
			MadMax::GetTimeOfDay(hour);
			SetDaylight(fc, hour);
			fc.fog[0] = fogStart;
			fc.fog[1] = fogEnd;
			fc.fog[2] = fogMax;
			fc.fog[3] = fogMax > 0.0f ? 1.0f : 0.0f;
			const Vec3 one = MadMax::FromMc(1.0, 0.0, 1.0);
			fc.axes[0] = one.x < 0.0f ? -1.0f : 1.0f;
			fc.axes[2] = one.z < 0.0f ? -1.0f : 1.0f;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(a_context->Map(frameCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				std::memcpy(mapped.pData, &fc, sizeof(fc));
				a_context->Unmap(frameCb, 0);
			}

			// Target: the back buffer, depth-tested against Mad Max's scene depth when we have it.
			ID3D11Texture2D* backBuffer = nullptr;
			if (FAILED(a_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
				return;
			}
			D3D11_TEXTURE2D_DESC bbDesc{};
			backBuffer->GetDesc(&bbDesc);
			const bool              haveScene = sceneLighting && CopyScene(device, a_context, backBuffer, bbDesc);
			ID3D11RenderTargetView* rtv = nullptr;
			const auto              hr = device->CreateRenderTargetView(backBuffer, nullptr, &rtv);
			Release(backBuffer);
			if (FAILED(hr)) {
				return;
			}
			if (haveScene) {
				fc.light[3] = 1.0f;
				if (SUCCEEDED(a_context->Map(frameCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
					std::memcpy(mapped.pData, &fc, sizeof(fc));
					a_context->Unmap(frameCb, 0);
				}
			}
			DepthCandidate sceneDepth{};
			{
				std::lock_guard g{ depthLock };
				if (!frameDepths.empty()) {
					sceneDepth = frameDepths.front();
					sceneDepth.dsv->AddRef();
				}
			}
			const bool reversed = sceneDepth.dsv ? sceneDepth.clear < 0.5f : false;
			if (!sceneDepth.dsv && !EnsureOwnDepth(bbDesc.Width, bbDesc.Height)) {
				Release(rtv);
				return;
			}
			ID3D11DepthStencilView* dsv = sceneDepth.dsv ? sceneDepth.dsv : ownDsv;
			if (!sceneDepth.dsv) {
				a_context->ClearDepthStencilView(ownDsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
			}
			static int loggedDepth = -1;
			if (loggedDepth != (sceneDepth.dsv ? 1 : 0)) {
				loggedDepth = sceneDepth.dsv ? 1 : 0;
				logger::info("world renderer: {}", sceneDepth.dsv ? std::format("depth-testing against Mad Max's scene ({} Z)", reversed ? "reversed" : "standard")
				                                                 : std::string("no scene depth found: Minecraft things draw over Mad Max's world"));
			}

			// Entities, cracks and the outline around an integer origin near the camera.
			const auto camMc = MadMax::ToMc(camPos);
			const double o[3] = { std::floor(camMc.x), std::floor(camMc.y), std::floor(camMc.z) };
			dynVerts.clear();
			static std::vector<Vertex> cracks;
			cracks.clear();
			BuildEntities(o, cracks);
			const auto entityVerts = static_cast<UINT>(dynVerts.size());
			dynVerts.insert(dynVerts.end(), cracks.begin(), cracks.end());
			const auto crackVerts = static_cast<UINT>(cracks.size());
			BuildOutline(o);
			const auto outlineVerts = static_cast<UINT>(dynVerts.size()) - entityVerts - crackVerts;
			const bool haveDyn = UploadDynamic(a_context);

			StateBackup saved;
			saved.Save(a_context);
			D3D11_VIEWPORT view{ 0, 0, float(bbDesc.Width), float(bbDesc.Height), 0, 1 };
			a_context->RSSetViewports(1, &view);
			a_context->RSSetState(raster);
			a_context->OMSetRenderTargets(1, &rtv, dsv);
			ID3D11Buffer* cbs[2] = { frameCb, objectCb };
			a_context->VSSetConstantBuffers(0, 2, cbs);
			a_context->PSSetConstantBuffers(0, 2, cbs);
			ID3D11SamplerState* samplerPair[2] = { atlasSampler, smoothSampler };
			a_context->PSSetSamplers(0, 2, samplerPair);
			a_context->IASetInputLayout(layout);
			a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			a_context->VSSetShader(vs, nullptr, 0);
			a_context->PSSetShader(ps, nullptr, 0);
			a_context->PSSetShaderResources(0, 1, &atlasSrv);
			ID3D11ShaderResourceView* sceneView = haveScene ? sceneSrv : nullptr;
			a_context->PSSetShaderResources(1, 1, &sceneView);
			const UINT stride = sizeof(Vertex), zero = 0;

			// Opaque and cutout blocks, Steve and other entities.
			a_context->OMSetBlendState(noBlend, nullptr, 0xFFFFFFFF);
			a_context->OMSetDepthStencilState(reversed ? depthWriteRev : depthWrite, 0);
			for (auto& [key, s] : sections) {
				const double dx = s.sx * 16.0 + 8.0 - camMc.x, dy = s.sy * 16.0 + 8.0 - camMc.y, dz = s.sz * 16.0 + 8.0 - camMc.z;
				if (!s.opaque || dx * dx + dy * dy + dz * dz > 320.0 * 320.0) {
					continue;
				}
				const double origin[3] = { s.sx * 16.0, s.sy * 16.0, s.sz * 16.0 };
				SetObjectOffset(a_context, origin);
				a_context->IASetVertexBuffers(0, 1, &s.vb, &stride, &zero);
				a_context->Draw(s.opaque, 0);
			}
			DrawEntities(a_context, false);
			if (haveDyn && entityVerts) {
				SetObjectOffset(a_context, o);
				a_context->IASetVertexBuffers(0, 1, &dynVb, &stride, &zero);
				a_context->Draw(entityVerts, 0);
			}

			// Water, stained glass, cracks, the outline: blended, no depth writes.
			a_context->OMSetBlendState(alphaBlend, nullptr, 0xFFFFFFFF);
			a_context->OMSetDepthStencilState(reversed ? depthReadRev : depthRead, 0);
			a_context->RSSetState(rasterCullBack);
			for (auto& [key, s] : sections) {
				if (!s.translucent) {
					continue;
				}
				const double origin[3] = { s.sx * 16.0, s.sy * 16.0, s.sz * 16.0 };
				SetObjectOffset(a_context, origin);
				a_context->IASetVertexBuffers(0, 1, &s.vb, &stride, &zero);
				a_context->Draw(s.translucent, s.opaque);
			}
			a_context->RSSetState(raster);
			DrawEntities(a_context, true);
			if (haveDyn && crackVerts) {
				SetObjectOffset(a_context, o);
				a_context->IASetVertexBuffers(0, 1, &dynVb, &stride, &zero);
				a_context->Draw(crackVerts, entityVerts);
			}
			if (haveDyn && outlineVerts) {
				SetObjectOffset(a_context, o);
				a_context->IASetVertexBuffers(0, 1, &dynVb, &stride, &zero);
				a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
				a_context->Draw(outlineVerts, entityVerts + crackVerts);
			}

			saved.Restore(a_context);
			Release(sceneDepth.dsv);
			Release(rtv);
		}
	}

	namespace WorldRender
	{
		void Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, IDXGISwapChain* a_swapChain)
		{
			DXGI_SWAP_CHAIN_DESC desc{};
			if (SUCCEEDED(a_swapChain->GetDesc(&desc))) {
				screenW = desc.BufferDesc.Width;
				screenH = desc.BufferDesc.Height;
			}
			if (!Init(a_device)) {
				return;
			}
			HookDepthClears(a_context);
			DrainMessages(a_context);
			auto& link = Link::Get();
			if (!link.ReadWorldEntities(entities)) {
				entities.count = 0;
				entities.hasSelection = 0;
			}
			if (link.McAlive() && State().mcInWorld && atlasSrv) {
				Render(a_context, a_swapChain);
			}
			// Next frame's depth candidates start fresh.
			std::lock_guard g{ depthLock };
			for (auto& d : frameDepths) {
				Release(d.dsv);
			}
			frameDepths.clear();
		}
	}
}
