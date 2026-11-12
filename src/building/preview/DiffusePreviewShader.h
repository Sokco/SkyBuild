#pragma once
#include "rendering/SceneShader.h"
#include <string>
namespace SkyBuild::DiffusePreview {
inline const std::string shader = std::string(Rendering::sceneShader) + R"SHADER(
cbuffer BrushData : register(b0) {
  row_major float4x4 modelToWorld;
  float4 tint;
  float4 alphaData; // blend alpha, test function, threshold, textured
  float4 uvTransform;
  float4 params; // bias in game units, emissive tint strength
};
Texture2D<float4> diffuse : register(t1);
SamplerState diffuseSampler : register(s0);
struct Fragment {
  float4 position : SV_POSITION;
  float3 world : TEXCOORD0;
  float2 uv : TEXCOORD2;
  noperspective float4 scene : TEXCOORD1; // NDC and reciprocal clip W.
};
Fragment VS(float3 local : POSITION, float2 uv : TEXCOORD0) {
  float3 p = mul(modelToWorld,float4(local,1)).xyz;
  Fragment o;
  o.uv=uv*uvTransform.xy+uvTransform.zw;
  ProjectWorld(p, o.world, o.position, o.scene);
  return o;
}
struct Result {float4 color : SV_TARGET;float depth : SV_DEPTH;};
Result PS(Fragment i) {
  float4 tex=alphaData.w>0.5?diffuse.Sample(diffuseSampler,i.uv):float4(1,1,1,1);
  int test=(int)alphaData.y;
  float a=tex.a, ref=alphaData.z;
  bool alphaPass=test<0 || test==0 || (test==1 && a<ref) || (test==2 && a==ref) ||
      (test==3 && a<=ref) || (test==4 && a>ref) || (test==5 && a!=ref) || (test==6 && a>=ref);
  if(!alphaPass)discard;
  float opacity=tint.a*(alphaData.x>0.5?tex.a:1);
  clip(opacity-0.001);
  float coverage = SceneCoverage(i.scene, params.x);
  clip(coverage-0.01);
  Result result;
  // Native preview tint used emissive color added to diffuse lighting before
  // multiplication by albedo. Preserve texture detail instead of blending
  // toward a flat color. Unit diffuse replaces the native lighting here.
  result.color=float4(tex.rgb * (1 + tint.rgb * params.y),opacity*coverage);
  // Keep the private depth target forward-Z even when scene depth is reversed.
  result.depth = SceneDepthReversed() ? 1 - i.position.z : i.position.z;
  return result;
}
)SHADER";
}
