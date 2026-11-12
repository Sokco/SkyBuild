#pragma once
#include <cstdint>
#include <d3d11.h>
#include <wrl/client.h>

namespace SkyBuild::Rendering::WorldFrame {
struct Frame {
  Microsoft::WRL::ComPtr<ID3D11Buffer> camera;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depth;
  std::uint32_t number{};
};
bool Install();
enum class Consumer : unsigned { ObjectPreview = 1, TerrainBrush = 2 };
void SetActive(Consumer consumer, bool active);
const Frame *Current();
void EndFrame();
}
