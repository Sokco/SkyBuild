#pragma once
#include "ShellVoxelizer.h"
#include <d2d1.h>
#include <wrl/client.h>

namespace SkyBuild::MaterialShell {
struct FootprintResult {
  double woodBudget{};
  std::map<std::string, double> textures;
};

// All triangles have the same winding in projection. Nonzero fill therefore
// measures their union, including overlaps between front and back faces.
inline FootprintResult AnalyzeFootprint(const Mesh &mesh,
                                        const std::atomic_bool &cancel) {
  using Microsoft::WRL::ComPtr;
  const auto check = [](HRESULT hr) {
    if (FAILED(hr))
      throw std::runtime_error(
          "Unable to measure the projected material footprint.");
  };
  const auto checkCancelled = [&] {
    if (cancel.load())
      throw std::runtime_error("Material analysis cancelled.");
  };
  std::array<double, 3> low{INFINITY, INFINITY, INFINITY},
      high{-INFINITY, -INFINITY, -INFINITY};
  std::map<std::string, std::vector<const Triangle *>> surfaces;
  for (const auto &triangle : mesh.triangles) {
    checkCancelled();
    if (triangle.texture >= mesh.textures.size())
      throw std::runtime_error("Invalid texture assignment.");
    const auto &p = triangle.points;
    for (const auto &vertex : p)
      for (int axis = 0; axis < 3; ++axis)
        if (!std::isfinite(vertex[axis]))
          throw std::runtime_error("Invalid mesh vertex.");
    const auto normal = Cross(p[1] - p[0], p[2] - p[0]);
    if (Dot(normal, normal) == 0.)
      continue;
    surfaces[mesh.textures[triangle.texture]].push_back(&triangle);
    for (const auto &vertex : p)
      for (int axis = 0; axis < 3; ++axis) {
        low[axis] = std::min(low[axis], vertex[axis]);
        high[axis] = std::max(high[axis], vertex[axis]);
      }
  }
  if (surfaces.empty())
    throw std::runtime_error("No triangle surfaces available.");
  std::array<double, 3> size{};
  for (int i = 0; i < 3; ++i)
    size[i] = high[i] - low[i];
  std::sort(size.begin(), size.end());
  FootprintResult result;
  result.woodBudget = std::clamp(
      std::round((2. * size[0] + size[1] + size[2]) / 128.), 1., 1000000.);

  ComPtr<ID2D1Factory> factory;
  check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                          factory.GetAddressOf()));
  for (int u = 0; u < 3; ++u) {
    const int v = (u + 1) % 3;
    std::vector<ComPtr<ID2D1PathGeometry>> paths;
    std::vector<ID2D1Geometry *> geometries;
    std::map<std::string, double> areas;
    double totalArea = 0.;
    for (const auto &[texture, triangles] : surfaces) {
      ComPtr<ID2D1PathGeometry> path;
      ComPtr<ID2D1GeometrySink> sink;
      check(factory->CreatePathGeometry(&path));
      check(path->Open(&sink));
      sink->SetFillMode(D2D1_FILL_MODE_WINDING);
      for (const auto *triangle : triangles) {
        checkCancelled();
        D2D1_POINT_2F p[3];
        for (int i = 0; i < 3; ++i) {
          const auto &vertex = triangle->points[i];
          p[i] = {static_cast<float>(vertex[u] - low[u]),
                  static_cast<float>(vertex[v] - low[v])};
        }
        const double winding = double(p[1].x - p[0].x) * (p[2].y - p[0].y) -
                               double(p[1].y - p[0].y) * (p[2].x - p[0].x);
        if (winding == 0.)
          continue;
        if (winding < 0.)
          std::swap(p[1], p[2]);
        sink->BeginFigure(p[0], D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLines(p + 1, 2);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
      }
      check(sink->Close());
      float area{};
      check(path->ComputeArea(nullptr, &area));
      if (area > 0.) {
        geometries.push_back(path.Get());
        paths.push_back(std::move(path));
        areas[texture] = area;
        totalArea += area;
      }
    }
    if (areas.empty())
      continue;
    double planeArea = totalArea;
    if (areas.size() > 1) {
      checkCancelled();
      ComPtr<ID2D1GeometryGroup> group;
      check(factory->CreateGeometryGroup(
          D2D1_FILL_MODE_WINDING, geometries.data(),
          static_cast<UINT32>(geometries.size()), &group));
      float area{};
      check(group->ComputeArea(nullptr, &area));
      planeArea = area;
    }
    for (const auto &[texture, area] : areas)
      result.textures[texture] += planeArea * area / totalArea;
  }
  if (result.textures.empty())
    throw std::runtime_error("No measurable material footprint.");
  return result;
}
} // namespace SkyBuild::MaterialShell
