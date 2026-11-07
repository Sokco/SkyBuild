#include "naming/ReferenceNames.h"
#include "building/BuildingManager.h"
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
namespace SkyBuild::Naming {
namespace {
std::atomic<std::shared_ptr<const ReferenceNames>> referenceNames{
    std::make_shared<const ReferenceNames>()};
std::atomic_bool hasReferenceNames{};
using NameLookup = const char *(*)(RE::TESObjectREFR *);
NameLookup originalNameLookup{};
bool nameHookInstalled{};
const char *DisplayName(RE::TESObjectREFR *object) {
  if (object && hasReferenceNames.load(std::memory_order_relaxed)) {
    // Immutable snapshots avoid locking the rename UI against native readers.
    // Pin the returned string through the caller's use of the const char*.
    thread_local std::shared_ptr<const ReferenceNames> pinned;
    pinned = referenceNames.load();
    const auto found = pinned->find(object->GetFormID());
    if (found != pinned->end())
      return found->second.c_str();
  }
  return originalNameLookup(object);
}
void PublishNames(std::shared_ptr<const ReferenceNames> names) {
  const bool any = !names->empty();
  referenceNames.store(std::move(names));
  hasReferenceNames.store(any);
}
void InstallNameHook() {
  if (nameHookInstalled)
    return;
  const auto site =
      REL::Relocation<std::uintptr_t>{RELOCATION_ID(19354, 19781)}.address();
  // Five bytes of complete, position-independent instructions, verified on
  // 1.6.1170. Never overwrite another mod's detour or an unknown prologue.
  constexpr std::array<std::uint8_t, 5> prologue{0x40, 0x53, 0x55, 0x56, 0x57};
  if (std::memcmp(reinterpret_cast<const void *>(site), prologue.data(),
                  prologue.size()) != 0) {
    spdlog::warn(
        "Case-preserving names unavailable: modified or unsupported name lookup.");
    return;
  }
  static SKSE::Trampoline trampoline{"SkyBuild reference names"};
  trampoline.create(64);
  auto *resume = static_cast<std::uint8_t *>(trampoline.allocate(19));
  std::memcpy(resume, prologue.data(), 5);
  constexpr std::array<std::uint8_t, 6> jump{0xFF, 0x25, 0, 0, 0, 0};
  std::memcpy(resume + 5, jump.data(), 6);
  const std::uintptr_t continuation = site + 5;
  std::memcpy(resume + 11, &continuation, sizeof(continuation));
  originalNameLookup = reinterpret_cast<NameLookup>(resume);
  trampoline.write_branch<5>(site, DisplayName);
  nameHookInstalled = true;
}

} // namespace
void Initialize() { InstallNameHook(); }
void Reset() { PublishNames(std::make_shared<const ReferenceNames>()); }
void Set(RE::FormID id, const std::string &name) {
  if (!nameHookInstalled)
    return;
  const auto current = referenceNames.load();
  if (const auto found = current->find(id);
      found != current->end() && found->second == name)
    return;
  auto names = std::make_shared<ReferenceNames>(*current);
  (*names)[id] = name;
  PublishNames(std::move(names));
}
void Save(SKSE::SerializationInterface *serialization) {
  ReferenceNames savedNames;
  for (const auto &[id, name] : *referenceNames.load())
    if (BuildingManager::IsPlacedObject(id))
      savedNames.emplace(id, name);
  const auto namesData = EncodeNames(savedNames).dump();
  if (!serialization->OpenRecord(NamesSaveRecord, 1) ||
      !serialization->WriteRecordData(
          namesData.data(), static_cast<std::uint32_t>(namesData.size())))
    spdlog::error("Could not save reference names.");
}
void Load(SKSE::SerializationInterface *serialization, std::uint32_t version,
          std::uint32_t length) {
  if (version != 1 || length > 16 * 1024 * 1024) {
    spdlog::warn("Unsupported reference name record.");
    return;
  }
  std::string data(length, '\0');
  if (serialization->ReadRecordData(data.data(), length) != length)
    return;
  try {
    auto names = std::make_shared<ReferenceNames>();
    for (const auto &[id, name] : DecodeNames(nlohmann::json::parse(data))) {
      RE::FormID resolved{};
      if (serialization->ResolveFormID(id, resolved))
        names->emplace(resolved, name);
    }
    PublishNames(std::move(names));
  } catch (const std::exception &error) {
    spdlog::error("Invalid reference names: {}", error.what());
  }
}
} // namespace SkyBuild::Naming
