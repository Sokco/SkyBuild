#pragma once
namespace SkyBuild::DiffusePreview {
inline constexpr char shader[] = R"SHADER(
cbuffer BrushData : register(b0) {
  row_major float4x4 worldToClip;
  row_major float4x4 modelToWorld;
  float4 tint;
  float4 alphaData; // blend alpha, test function, threshold, textured
  float4 uvTransform;
  float4 params; // bias in game units, emissive tint strength
  float4 viewport; // target width/height, depth active width/height
  float4 depthJitter; // scene depth UV jitter; overlay geometry stays unjittered
};
// Standard scene PerFrame layout (engine/Community/Bottled Shaders b12).
// Borrow it at b1 in this private pass; never modify the game buffer.
cbuffer SceneFrame : register(b1) {
  row_major float4x4 sceneView : packoffset(c0);
  row_major float4x4 sceneProj : packoffset(c4);
  row_major float4x4 sceneViewProj : packoffset(c8);
  row_major float4x4 sceneViewProjUnjittered : packoffset(c12);
  float4 sceneCameraPosition : packoffset(c40);
  float4 sceneDynamicResolution : packoffset(c43);
};
Texture2D<float> sceneDepth : register(t0);
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
  o.world = depthJitter.z > 0.5 ? p - sceneCameraPosition.xyz : p;
  o.position = depthJitter.z > 0.5
      ? mul(sceneViewProjUnjittered, float4(o.world,1))
      : mul(worldToClip, float4(p, 1));
  // Project once at vertices and interpolate NDC in screen space. Reprojecting
  // perspective-interpolated world positions per pixel introduces a second
  // rounded projection, especially on grazing triangles.
  precise float4 sceneClip = depthJitter.z > 0.5
      ? mul(sceneViewProj, float4(o.world,1)) : o.position;
  o.scene = float4(sceneClip.xyz,1) / sceneClip.w;
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
  uint w, h;
  sceneDepth.GetDimensions(w, h);
  float2 sampleUV;
  float expectedDepth;
  bool reversed = depthJitter.w > 0.5;
  float bias = 0.00003;
  float2 activeSize = viewport.zw;
  if (depthJitter.z > 0.5) {
    // Project the SAME camera-relative position with the actual jittered GPU
    // matrix that wrote scene depth, not NiCamera plus an estimated offset.
    float3 actualNDC = i.scene.xyz;
    sampleUV = actualNDC.xy * float2(0.5,-0.5) + 0.5;
    expectedDepth = actualNDC.z;
    float facing = sceneProj[3][2] < 0 ? -1 : 1;
    float4 closeClip = mul(sceneProj,float4(0,0,facing,1));
    float4 distantClip = mul(sceneProj,float4(0,0,2*facing,1));
    reversed = distantClip.z * closeClip.w < closeClip.z * distantClip.w;
    // Convert the configurable camera-facing bias from game units to projected depth.
    float derivative = abs(sceneProj[2][2] * sceneProj[3][3] -
                           sceneProj[2][3] * sceneProj[3][2]);
    bias = clamp(params.x * derivative * i.scene.w * i.scene.w,
                 0.000002,0.0002);
    activeSize = float2(w,h) * sceneDynamicResolution.xy;
  } else {
    sampleUV = i.position.xy / viewport.xy + depthJitter.xy;
    expectedDepth = i.position.z;
  }
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
  // A depth test, not an equality window: surfaces nearer than scene depth
  // remain visible. Only preview surfaces hidden behind scene depth are rejected, with
  // a small camera-facing bias and a soft transition at its far limit.
  float coverage = smoothstep(-bias,-0.5*bias,clearance);
  clip(coverage-0.01);
  Result result;
  // Native preview tint used emissive color added to diffuse lighting before
  // multiplication by albedo. Preserve texture detail instead of blending
  // toward a flat color. Unit diffuse replaces the native lighting here.
  result.color=float4(tex.rgb * (1 + tint.rgb * params.y),opacity*coverage);
  // Reciprocal clip W gives this private depth buffer one convention on
  // vanilla and reversed-depth renderers. It never writes scene depth.
  result.depth=saturate(i.scene.w);
  return result;
}
)SHADER";
}
