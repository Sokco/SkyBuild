#pragma once
namespace SkyBuild::Rendering {
inline constexpr char sceneShader[] = R"SHADER(
// Private camera constants captured alongside the world depth snapshot.
cbuffer SceneFrame : register(b1) {
  row_major float4x4 sceneView : packoffset(c0);
  row_major float4x4 sceneProj : packoffset(c4);
  row_major float4x4 sceneViewProj : packoffset(c8);
  row_major float4x4 sceneViewProjUnjittered : packoffset(c12);
  float4 sceneCameraPosition : packoffset(c40);
  float4 sceneDynamicResolution : packoffset(c43);
};
Texture2D<float> sceneDepth : register(t0);

void ProjectWorld(float3 p, out float3 relative, out float4 position, out float4 scene) {
  relative = p - sceneCameraPosition.xyz;
  position = mul(sceneViewProjUnjittered, float4(relative,1));
  // Interpolate projected depth in screen space to match rasterization.
  precise float4 sceneClip = mul(sceneViewProj, float4(relative,1));
  scene = float4(sceneClip.xyz,1) / sceneClip.w;
}
bool SceneDepthReversed() {
  float facing = sceneProj[3][2] < 0 ? -1 : 1;
  float4 closeClip = mul(sceneProj,float4(0,0,facing,1));
  float4 distantClip = mul(sceneProj,float4(0,0,2*facing,1));
  return distantClip.z * closeClip.w < closeClip.z * distantClip.w;
}
float SceneCoverage(float4 projected, float worldBias) {
  uint w, h;
  sceneDepth.GetDimensions(w, h);
  float2 sampleUV = projected.xy * float2(0.5,-0.5) + 0.5;
  float expectedDepth = projected.z;
  bool reversed = SceneDepthReversed();
  float derivative = abs(sceneProj[2][2] * sceneProj[3][3] -
                         sceneProj[2][3] * sceneProj[3][2]);
  float bias = clamp(worldBias * derivative * projected.w * projected.w, 0.000002,0.0002);
  float2 activeSize = float2(w,h) * sceneDynamicResolution.xy;
  float2 depthCoord = sampleUV * activeSize;
  int2 first = int2(floor(depthCoord - 0.5));
  // Derivatives in depth-texture coordinates, accounting for both jittered
  // projection and render scaling. Avoid a screen-size approximation.
  float2 uvDx = ddx(depthCoord), uvDy = ddy(depthCoord);
  float zDx = ddx(expectedDepth), zDy = ddy(expectedDepth);
  float determinant = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
  float2 slope = abs(determinant) > 1e-8
      ? float2(zDx * uvDy.y - zDy * uvDx.y, uvDx.x * zDy - uvDy.x * zDx) / determinant
      : float2(0,0);
  float clearance = -1e10;
  // Compare the local depth plane at each actual depth texel center. This
  // handles subpixel jitter/scaling without interpolating foreground depth
  // across an occlusion edge or enlarging the accepted depth range.
  [unroll] for (int y=0; y<2; ++y)
    [unroll] for (int x=0; x<2; ++x) {
      int2 pixel = clamp(first + int2(x,y), int2(0,0), int2(w,h)-1);
      float expected = expectedDepth + dot(slope, float2(pixel) + 0.5 - depthCoord);
      float depth = sceneDepth.Load(int3(pixel,0));
      float separation = reversed ? expected - depth : depth - expected;
      clearance = max(clearance,separation);
    }
  float coverage = smoothstep(-bias,-0.5*bias,clearance);
  return coverage;
}
)SHADER";
}
