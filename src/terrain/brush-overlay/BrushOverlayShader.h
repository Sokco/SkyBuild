#pragma once
#include "rendering/SceneShader.h"
#include <string>
namespace SkyBuild::Terrain::BrushOverlay {
inline const std::string shader = std::string(Rendering::sceneShader) + R"(
cbuffer BrushData : register(b0) {
  float4 brush;
};
struct Fragment {
  float4 position : SV_POSITION;
  float3 world : TEXCOORD0;
  noperspective float4 scene : TEXCOORD1; // NDC and reciprocal clip W.
};
Fragment VS(float3 p : POSITION) {
  Fragment o;
  ProjectWorld(p, o.world, o.position, o.scene);
  return o;
}
float4 PS(Fragment i) : SV_TARGET {
  float3 center = brush.xyz - sceneCameraPosition.xyz;
  float distance = length(i.world - center) / brush.w;
  clip(1 - distance);
  float coverage = SceneCoverage(i.scene, 0.75);
  float edge = (1 - smoothstep(0.86, 1, distance));
  float rim = smoothstep(0.78, 0.92, distance);
  return float4(coverage * edge * (0.24 + rim * 0.25), 0, 0, 1);
}
float4 CompositeVS(uint id : SV_VertexID) : SV_POSITION {
  return float4(float2((id << 1) & 2, id & 2) * float2(2,-2) + float2(-1,1),0,1);
}
float4 CompositePS(float4 p : SV_POSITION) : SV_TARGET {
  return float4(0.08, 0.46, 1, sceneDepth.Load(int3(int2(p.xy),0)));
}
)";
}
