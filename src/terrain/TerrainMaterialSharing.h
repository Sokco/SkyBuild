#pragma once
#include <stdexcept>
namespace SkyBuild::Terrain {
// The source is already registered and owned by an engine shader property.
// SetMaterial(non-null, false) is a hash-map Link operation, not an AddRef:
// it can clone/canonicalize unique extension materials. Retain the exact
// registered object instead, and let the property's normal native teardown
// release that reference through the same material manager.
template <class Property, class Material>
void RetainTerrainMaterial(Property &property, Material *source) {
  if (!source)
    throw std::runtime_error("Missing source terrain material");
  if (property.material == source)
    return;
  source->IncRef();
  property.SetMaterial(nullptr, false); // Native release of any old material.
  property.material = source;
}
} // namespace SkyBuild::Terrain
