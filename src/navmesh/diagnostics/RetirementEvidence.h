#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>
namespace SkyBuild::Navmesh {
// Schema 1 is a 1.6.1170 native before/after witness, never portable save data.
// Validate owned bytes only: no captured address is dereferenced during replay.
inline std::string ValidateRetirementWitnesses(const nlohmann::json &d) {
  try {
    const auto &forms = d.at("forms");
    if (!forms.is_array() || forms.empty() || forms.size() > 128)
      return "Invalid retirement witness cohort";
    if (!d.at("beforeTopology").is_array() ||
        d.at("beforeTopology") != d.at("afterTopology") ||
        d.at("beforeTopology").size() != forms.size())
      return "Geometry/metadata topology rollback mismatch";
    std::set<std::uint32_t> ids, topologyIDs;
    std::set<std::uint64_t> formTokens, infoTokens;
    for (const auto &mesh : d.at("beforeTopology"))
      if (!topologyIDs.insert(mesh.at("id").get<std::uint32_t>()).second)
        return "Duplicate topology identity";
    const auto word = [](const auto &bytes, std::size_t at, unsigned size) {
      std::uint64_t result{};
      for (unsigned i = 0; i < size; ++i)
        result |= std::uint64_t(bytes[at + i]) << (8 * i);
      return result;
    };
    for (const auto &e : forms) {
      const auto id = e.at("id").get<std::uint32_t>();
      const auto form = e.at("form").get<std::uint64_t>(),
                 info = e.at("info").get<std::uint64_t>();
      for (const auto *field :
           {"formBefore", "formAfter", "infoBefore", "infoAfter"}) {
        if (!e.at(field).is_array())
          return "Invalid native byte array";
        for (const auto &byte : e.at(field))
          if (!byte.is_number_integer() || byte.get<std::int64_t>() < 0 ||
              byte.get<std::uint64_t>() > 255)
            return "Native byte value outside range";
      }
      const auto f = e.at("formBefore").get<std::vector<unsigned char>>();
      const auto n = e.at("infoBefore").get<std::vector<unsigned char>>();
      if (f.size() != 0x140 || n.size() != 0x48 ||
          e.at("formBefore") != e.at("formAfter") ||
          e.at("infoBefore") != e.at("infoAfter"))
        return "Native bytes/allocation rollback mismatch";
      if ((id >> 24) != 0xFF || !form || !info || !ids.insert(id).second ||
          !formTokens.insert(form).second || !infoTokens.insert(info).second ||
          word(f, 0x14, 4) != id || word(n, 0, 4) != id ||
          word(n, 0x28, 8) != form + 0x28 || word(f, 0x138, 8) != info ||
          word(f, 0x38, 8) != e.at("vertexAllocation").get<std::uint64_t>() ||
          word(f, 0x50, 8) != e.at("triangleAllocation").get<std::uint64_t>() ||
          word(f, 0x48, 4) != e.at("vertices").get<std::uint32_t>() ||
          word(f, 0x60, 4) != e.at("triangles").get<std::uint32_t>())
        return "Native identity/backlink/allocation witness mismatch";
    }
    if (ids != topologyIDs)
      return "Topology identity set mismatch";
    return {};
  } catch (const std::exception &e) {
    return std::string("Malformed native witness: ") + e.what();
  }
}
inline std::string ValidateRetirementEvidence(const nlohmann::json &d) {
  try {
    if (d.at("schema") != 1 || d.at("kind") != "retired-core-roundtrip" ||
        d.at("stage") != "exact-rollback-verified" ||
        !d.at("complete").get<bool>() || d.at("formsDestroyed") != 0 ||
        d.at("geometryPermanentlyReleased").get<bool>() ||
        d.at("passes") != 2 || d.at("injectedRollbacks") != 1)
      return "Incomplete or unsupported retirement proof";
    const auto &forms = d.at("forms");
    if (!forms.is_array() || forms.empty() || forms.size() > 65536 ||
        d.at("rejectedLocations") != forms.size() * 3 ||
        d.at("guardBefore") != d.at("guardAfter"))
      return "Missing cohort or stale-location/guard rollback coverage";
    return ValidateRetirementWitnesses(d);
  } catch (const std::exception &e) {
    return std::string("Malformed retirement witness: ") + e.what();
  }
}
} // namespace SkyBuild::Navmesh
