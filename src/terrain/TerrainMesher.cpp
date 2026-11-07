#include "../../extern/mc33/include/MC33.h"
#include "terrain/TerrainVolume.h"

namespace SkyBuild::Terrain {
Mesh Polygonize(const Volume &volume, int startX, int startY, int width) {
  if (!volume.Valid() || startX < 0 || startY < 0 || width < 1 ||
      startX + width >= Side || startY + width >= Side)
    throw std::runtime_error("Invalid terrain chunk");
  const int depth = int(std::lround((volume.top - volume.bottom) / Step));
  const int stride = width + 1;
  std::vector<GRD_data_type> samples(std::size_t(stride) * stride *
                                     (depth + 1));
  auto local = volume;
  {
    if (volume.density) {
      const auto &field = *volume.density->current;
      for (int z = 0; z <= depth; ++z)
        for (int y = 0; y < stride; ++y)
          for (int x = 0; x < stride; ++x)
            samples[(std::size_t(z) * stride + y) * stride + x] =
                field.At({int(volume.bottom / Step) + z,
                          std::uint32_t((startY + y + 1) * DensitySide +
                                        startX + x + 1)});
    } else {
      const auto lo = volume.origin + Vec{startX * Step - 1, startY * Step - 1,
                                          volume.bottom - 1};
      const auto hi =
          volume.origin + Vec{(startX + width) * Step + 1,
                              (startY + width) * Step + 1, volume.top + 1};
      local = LocalizedVolume(volume, lo, hi);
      std::vector<float> heights(std::size_t(stride) * stride);
      for (int y = 0; y < stride; ++y)
        for (int x = 0; x < stride; ++x)
          heights[y * stride + x] =
              local.Height(volume.origin.x + (startX + x) * Step,
                           volume.origin.y + (startY + y) * Step);
      for (int z = 0; z <= depth; ++z)
        for (int y = 0; y <= width; ++y)
          for (int x = 0; x <= width; ++x)
            samples[(std::size_t(z) * stride + y) * stride + x] =
                local.DensityAtHeight({volume.origin.x + (startX + x) * Step,
                                       volume.origin.y + (startY + y) * Step,
                                       volume.bottom + z * Step},
                                      heights[y * stride + x]);
    }
  }
  // Pass the unmodified field to the author's MC33 implementation. It owns
  // case selection, face/interior ambiguity tests, shared vertices, exact-zero
  // handling and triangle winding. No custom contour fans or hole filling.
  grid3d grid;
  if (grid.set_data_pointer(stride, stride, depth + 1, samples.data()) != 0)
    throw std::runtime_error("MC33 grid allocation failed");
  MC33 mesher;
  if (mesher.set_grid3d(grid) != 0)
    throw std::runtime_error("MC33 grid setup failed");
  surface generated;
  {
    if (mesher.calculate_isosurface(generated, 0) != 0)
      throw std::runtime_error("MC33 isosurface extraction failed");
  }
  if (generated.get_num_vertices() > 60000 ||
      generated.get_num_triangles() > 60000)
    throw std::runtime_error("Terrain chunk mesh budget exceeded");
  Mesh mesh;
  mesh.vertices.reserve(generated.get_num_vertices());
  mesh.triangles.reserve(generated.get_num_triangles());
  for (unsigned i = 0; i < generated.get_num_vertices(); ++i) {
    const auto p = generated.getVertex(i);
    const Vec world{static_cast<float>(double(volume.origin.x) +
                                       (double(startX) + p[0]) * Step),
                    static_cast<float>(double(volume.origin.y) +
                                       (double(startY) + p[1]) * Step),
                    static_cast<float>(double(volume.bottom) + p[2] * Step)};
    if (!Finite(world))
      throw std::runtime_error("Non-finite MC33 vertex");
    mesh.vertices.push_back({world, local.Normal(world)});
  }
  for (unsigned i = 0; i < generated.get_num_triangles(); ++i) {
    const auto t = generated.getTriangle(i);
    if (t[0] >= mesh.vertices.size() || t[1] >= mesh.vertices.size() ||
        t[2] >= mesh.vertices.size())
      throw std::runtime_error("Invalid MC33 triangle index");
    mesh.triangles.push_back({static_cast<std::uint16_t>(t[0]),
                              static_cast<std::uint16_t>(t[1]),
                              static_cast<std::uint16_t>(t[2])});
  }
  return mesh;
}
} // namespace SkyBuild::Terrain
