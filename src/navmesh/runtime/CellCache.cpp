#include "navmesh/runtime/CellCache.h"
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace SkyBuild::Navmesh {
namespace {
using Json = nlohmann::json;
bool SameMesh(const Mesh &a, const Mesh &b) {
  if (a.vertices != b.vertices || a.triangles.size() != b.triangles.size())
    return false;
  for (std::size_t i = 0; i < a.triangles.size(); ++i) {
    const auto &x = a.triangles[i], &y = b.triangles[i];
    if (x.vertices != y.vertices || x.neighbors != y.neighbors ||
        x.flags != y.flags || x.source.formID != y.source.formID ||
        x.source.triangle != y.source.triangle)
      return false;
  }
  return true;
}
Json EncodeMesh(const Mesh &mesh) {
  Json data{{"vertices", Json::array()}, {"triangles", Json::array()}};
  for (const auto &p : mesh.vertices)
    data["vertices"].push_back({p.x, p.y, p.z});
  for (const auto &t : mesh.triangles)
    data["triangles"].push_back(
        {t.vertices, t.neighbors, t.flags, t.source.formID, t.source.triangle});
  return data;
}
Mesh DecodeMesh(const Json &data) {
  Mesh mesh;
  const auto &vertices = data.at("vertices"), &triangles = data.at("triangles");
  if (!vertices.is_array() || !triangles.is_array() ||
      vertices.size() > 1000000 || triangles.size() > 1000000)
    throw std::runtime_error("Invalid navmesh cache geometry size");
  for (const auto &p : vertices) {
    if (p.size() != 3)
      throw std::runtime_error("Invalid cached vertex");
    Point point{p[0], p[1], p[2]};
    if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
        !std::isfinite(point.z))
      throw std::runtime_error("Nonfinite cached vertex");
    mesh.vertices.push_back(point);
  }
  for (const auto &t : triangles) {
    if (t.size() != 5)
      throw std::runtime_error("Invalid cached triangle");
    Triangle face;
    face.vertices = t[0].get<decltype(face.vertices)>();
    face.neighbors = t[1].get<decltype(face.neighbors)>();
    face.flags = t[2];
    face.source = {t[3], t[4]};
    for (const auto vertex : face.vertices)
      if (vertex >= mesh.vertices.size())
        throw std::runtime_error("Invalid cached vertex index");
    for (const auto neighbor : face.neighbors)
      if (neighbor < -1 ||
          (neighbor >= 0 && std::size_t(neighbor) >= triangles.size()))
        throw std::runtime_error("Invalid cached adjacency");
    mesh.triangles.push_back(face);
  }
  return mesh;
}
} // namespace
bool CacheOriginalsMatch(
    const CachedCell &cache,
    const std::vector<std::shared_ptr<const Baseline>> &originals,
    const std::map<std::uint32_t, std::set<std::uint32_t>> &doorFaces) {
  return CacheOriginalsMismatch(cache, originals, doorFaces).empty();
}
std::string CacheOriginalsMismatch(
    const CachedCell &cache,
    const std::vector<std::shared_ptr<const Baseline>> &originals,
    const std::map<std::uint32_t, std::set<std::uint32_t>> &doorFaces) {
  if (cache.originals.size() != originals.size())
    return "Original mesh count changed: " +
           std::to_string(cache.originals.size()) + " saved / " +
           std::to_string(originals.size()) + " fresh";
  for (const auto &source : originals) {
    if (!source)
      return "Fresh original baseline is missing";
    const auto found =
        std::find_if(cache.originals.begin(), cache.originals.end(),
                     [&](const auto &baseline) {
                       return baseline.formID == source->formID;
                     });
    const auto id = std::to_string(source->formID);
    if (found == cache.originals.end())
      return "Original form ID changed: " + id;
    const auto &saved = found->mesh, &fresh = source->mesh;
    if (SameMesh(saved, fresh))
      continue;
    if (saved.vertices.size() != fresh.vertices.size())
      return "Original " + id + " vertex count changed";
    for (std::size_t i = 0; i < saved.vertices.size(); ++i)
      if (saved.vertices[i] != fresh.vertices[i])
        return "Original " + id + " vertex " + std::to_string(i) + " moved";
    if (saved.triangles.size() != fresh.triangles.size())
      return "Original " + id + " triangle count changed";
    for (std::size_t i = 0; i < saved.triangles.size(); ++i) {
      const auto &a = saved.triangles[i], &b = fresh.triangles[i];
      const auto prefix = "Original " + id + " triangle " + std::to_string(i);
      if (a.vertices != b.vertices)
        return prefix + " vertex indices changed";
      if (a.neighbors != b.neighbors)
        return prefix + " neighbors changed";
      if (a.source.formID != b.source.formID ||
          a.source.triangle != b.source.triangle)
        return prefix + " source identity changed";
      // Native closed-door state owns bit1000 on explicitly witnessed door
      // faces. It is not a geometry edit. Never mask that bit globally or
      // accept another flag/provenance change alongside it.
      const auto witness = doorFaces.find(source->formID);
      const bool doorOverlay = (a.flags ^ b.flags) == 0x1000U &&
                               a.source.formID == source->formID &&
                               witness != doorFaces.end() &&
                               witness->second.contains(a.source.triangle);
      if (a.flags != b.flags && !doorOverlay)
        return prefix + " flags changed: " + std::to_string(a.flags) +
               " saved / " + std::to_string(b.flags) + " fresh";
    }
  }
  return {};
}
std::vector<std::uint8_t> EncodeCellCache(const CellCache &cache) {
  Json cells = Json::array();
  for (const auto &[id, cell] : cache) {
    Json data{{"id", id},
              {"originals", Json::array()},
              {"modified", EncodeMesh(cell.modified)},
              {"placements", Json::array()}};
    for (const auto &source : cell.originals)
      data["originals"].push_back({source.formID, EncodeMesh(source.mesh)});
    for (const auto &stamp : cell.placements)
      data["placements"].push_back(
          {stamp.reference, stamp.base, stamp.transform});
    cells.push_back(std::move(data));
  }
  auto bytes = Json::to_cbor(cells);
  if (bytes.size() > MAX_CELL_CACHE_BYTES)
    throw std::runtime_error("Navmesh save cache exceeds 64 MiB budget");
  return bytes;
}
CellCache DecodeCellCache(
    std::span<const std::uint8_t> bytes,
    const std::function<std::optional<std::uint32_t>(std::uint32_t)> &resolve) {
  if (bytes.size() > MAX_CELL_CACHE_BYTES)
    throw std::runtime_error("Oversized navmesh save cache");
  const auto cells = Json::from_cbor(bytes.begin(), bytes.end());
  if (!cells.is_array() || cells.size() > 4096)
    throw std::runtime_error("Invalid navmesh cache cell count");
  CellCache result;
  for (const auto &data : cells) {
    const auto cellID = resolve(data.at("id"));
    if (!cellID)
      continue;
    CachedCell cell;
    bool valid = true;
    std::set<std::uint32_t> sources;
    const auto &originals = data.at("originals"),
               &placements = data.at("placements");
    if (originals.size() > 1024 || placements.size() > 100000)
      throw std::runtime_error("Invalid navmesh cache entry size");
    for (const auto &entry : originals) {
      const auto id = resolve(entry.at(0));
      if (!id || !sources.insert(*id).second) {
        valid = false;
        break;
      }
      auto mesh = DecodeMesh(entry.at(1));
      for (auto &face : mesh.triangles) {
        if (face.source.Authored())
          face.source.formID = *id;
        else
          face.source.formID = 0;
      }
      cell.originals.push_back({*id, std::move(mesh)});
    }
    if (!valid)
      continue;
    cell.modified = DecodeMesh(data.at("modified"));
    for (auto &face : cell.modified.triangles) {
      if (!face.source.Authored()) {
        face.source.formID = 0;
        continue;
      }
      const auto id = resolve(face.source.formID);
      if (!id || !sources.contains(*id)) {
        valid = false;
        break;
      }
      face.source.formID = *id;
    }
    for (const auto &entry : placements) {
      const auto transform = entry.at(2).get<std::array<float, 7>>();
      if (!std::ranges::all_of(transform,
                               [](float v) { return std::isfinite(v); }))
        throw std::runtime_error("Nonfinite cached placement");
      const auto reference = resolve(entry.at(0)), base = resolve(entry.at(1));
      if (!reference || !base) {
        // Deleted references do not invalidate the last committed navigation.
        // These stamps describe its generation input, not live dependencies.
        continue;
      }
      PlacementStamp stamp{*reference, *base, transform};
      cell.placements.push_back(stamp);
    }
    std::ranges::sort(cell.placements, {}, &PlacementStamp::reference);
    if (valid && !result.emplace(*cellID, std::move(cell)).second)
      throw std::runtime_error("Duplicate cached cell");
  }
  return result;
}
} // namespace SkyBuild::Navmesh
