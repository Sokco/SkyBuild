#include "models/VertexBounds.h"
#include "building/materials/ProjectedFootprint.h"
#include "building/AutomaticBuildCosts.h"
#include "ui/pack-editor/PackEditor.h"
#include "building/BuildingMode.h"
#include "terrain/TerrainTool.h"
#include "building/PlaceableRegistry.h"
#include "building/PlacementPreview.h"
#include "ui/imgui/UI.h"
#include "ui/imgui/Widgets.h"
#include "ui/pack-editor/OfficialAssets.h"
#include "ui/pack-editor/PackAssets.h"
#include "ui/pack-editor/PackDocument.h"
#include "ui/pack-editor/PackEditorView.h"
#include "ui/pack-editor/PackZip.h"
#include "ui/settings/Settings.h"
#include "ui/thumbnails/ThumbnailGenerator.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <shobjidl.h>
#include <unordered_map>
#include <unordered_set>
#include <wrl/client.h>

namespace SkyBuild::UI::PackEditor {
namespace {
using Json = nlohmann::json;
using PackAssets::AssetPath;
using PackAssets::MeshReferences;
struct PickerRecord {
  RE::FormID id;
  std::uint32_t local;
  std::string name, editorID, plugin, model, search, winningPlugin,
      pluginSearch, recordType;
  bool hasFurnitureEquivalent{false};
};
struct Choice {
  std::string path;
  bool include{false}, available{true}, gameFile{false};
  std::vector<std::string> usedBy;
};
struct Result {
  std::string message;
  std::optional<Json> document;
  std::vector<std::filesystem::path> packs;
  std::vector<Choice> assets, plugins;
  bool reload{false}, discovered{false};
  std::optional<Json> addedItem;
};
struct MaterialRecord {
  RE::FormID id;
  std::uint32_t local;
  std::string plugin, name, editorID, nameSearch, editorSearch;
  bool selectable{};
};
std::vector<MaterialRecord> materialObjects;
std::deque<ThumbnailGenerator::IconRequest> resourceIcons;
std::unordered_set<std::string> requestedResourceIcons;
struct ResourceCapture {
  ThumbnailGenerator::IconRequest request;
  std::uint64_t generation;
};
std::optional<ResourceCapture> resourceCapture;

std::unordered_map<std::string, std::size_t> materialByIdentity;
std::vector<PickerRecord> searchObjects;
std::unordered_map<RE::FormID, std::size_t> worldPickObjects;
bool selectingWorldItem{}, selectionWasTerrain{};
BuildingMode::InteractionMode selectionReturnTool{
    BuildingMode::InteractionMode::kIdle};
bool restorePackPending{true};
std::string lastPackName;
bool editorIDsAvailable = false;
std::vector<std::string> objectPlugins;
std::unordered_map<std::string, std::vector<std::string>> masters;
std::unordered_map<std::string, RE::FormID> identities;
std::unordered_map<std::string, std::size_t> objectByIdentity;
std::uint64_t documentRevision = 0;
std::vector<std::filesystem::path> packs;
std::vector<Choice> assets, plugins;
std::uint64_t assetCatalogRevision = 0, sentAssetCatalogRevision = ~0ULL;
Json assetSelectionUpdate;
std::future<Result> job;
std::optional<Json> shellTarget;
std::deque<Json> costBatch;
std::deque<Json> boundsBatch;
std::size_t boundsTotal{}, boundsFailed{};
std::unordered_set<std::string> refreshedBoundsModels;
std::uint64_t shellEpoch{}, shellWorkerEpoch{}, shellGeneration{};
bool shellNeedsCapture{}, shellExtracting{};
std::shared_ptr<std::atomic_bool> shellCancel =
    std::make_shared<std::atomic_bool>(false);
std::future<MaterialShell::FootprintResult> shellWorker;
std::string shellStatus;
void CancelShell() {
  ++shellEpoch;
  shellCancel->store(true);
  shellTarget.reset();
  shellNeedsCapture = false;
  if (shellExtracting &&
      ThumbnailGenerator::GetProgress().generation == shellGeneration)
    ThumbnailGenerator::Cancel();
  shellExtracting = false;
  shellStatus.clear();
}
void RequestShell(Json target) {
  CancelShell();
  shellTarget = std::move(target);
  shellNeedsCapture = true;
  shellStatus = "Waiting to read triangle surfaces...";
}

std::filesystem::path selectedPath;
Json document;
std::optional<Json> pickedItem;
std::string message, itemFilter;
std::optional<std::string> itemCategoryFilter;

bool dirty = false, exporting = false;
bool closeSavePending = false;
std::filesystem::path Data() {
  return std::filesystem::current_path() / "Data";
}
void RememberPack() {
  const auto name = selectedPath.stem().string();
  if (name == lastPackName)
    return;
  try {
    const auto path = Data() / "SkyBuild/PackEditorState.json";
    auto temporary = path;
    temporary += ".tmp";
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << Json{{"lastPack", name}}.dump(2) << '\n';
    output.close();
    if (!output || !MoveFileExW(temporary.c_str(), path.c_str(),
                                MOVEFILE_REPLACE_EXISTING))
      throw std::runtime_error("Cannot write PackEditorState.json");
    lastPackName = name;
  } catch (const std::exception &error) {
    spdlog::warn("Could not remember Pack Editor selection: {}", error.what());
  }
}
std::string Identity(const std::string &plugin, std::uint32_t local) {
  return PackDocument::Lower(plugin) + std::format(":{:06X}", local);
}
std::uint32_t Local(const Json &item) {
  return item["formId"].is_string()
             ? ParseThumbnailFormID(item["formId"].get<std::string>())
                   .value_or(0)
             : item["formId"].get<std::uint32_t>();
}
std::vector<std::filesystem::path> PackPaths() {
  std::vector<std::filesystem::path> result;
  std::error_code error;
  const auto root = Data() / "SkyBuild/packs";
  std::filesystem::create_directories(root, error);
  if (error)
    throw std::runtime_error(error.message());
  for (std::filesystem::directory_iterator at(root, error), end;
       at != end && !error; ++at)
    if (at->is_regular_file() &&
        PackDocument::Lower(at->path().extension().string()) == ".json")
      result.push_back(at->path());
  if (error)
    throw std::runtime_error(error.message());
  std::ranges::sort(result);
  return result;
}
void WritePack(const std::filesystem::path &path, const Json &value,
               bool create = false) {
  auto saved = value;
  if (Packs::SchemaVersion(saved) != Packs::CurrentSchemaVersion)
    throw std::runtime_error("Pack must be migrated before saving.");
  if (Packs::EnsureIdentities(saved))
    throw std::runtime_error("Pack or item is missing its stable ID.");
  const auto validation = PackDocument::Validate(saved);
  if (!validation.empty())
    throw std::runtime_error(validation);
  Packs::BackupLegacyPack(path);
  auto temp = path;
  temp += L".tmp";
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    file << saved.dump(2) << '\n';
    file.close();
    if (!file)
      throw std::runtime_error("Could not write pack config.");
  }
  if (!MoveFileExW(temp.c_str(), path.c_str(),
                   create ? 0 : MOVEFILE_REPLACE_EXISTING)) {
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    throw std::runtime_error(create ? "A pack with that name already exists, "
                                      "or its folder is not writable."
                                    : "Could not replace pack config.");
  }
}
bool Available(const std::string &path) {
  RE::BSResourceNiBinaryStream stream(path);
  return stream.good();
}
std::vector<char> ReadMesh(const std::string &path) {
  RE::BSResourceNiBinaryStream stream(path);
  if (!stream.good() || !stream.stream)
    return {};
  const auto size = stream.stream->totalSize;
  if (size > 64 * 1024 * 1024)
    throw std::runtime_error("Mesh is too large to inspect: " + path);
  std::vector<char> data(size);
  if (size && !stream.read(data.data(), size))
    throw std::runtime_error("Could not inspect mesh: " + path);
  return data;
}

Choice AssetChoice(const std::string &path) {
  static std::unordered_map<std::string, Choice> checked;
  const auto key = PackDocument::Lower(path);
  if (const auto found = checked.find(key); found != checked.end())
    return found->second;
  Choice choice{path, false, Available(path), false};
  if (!choice.available)
    return choice;
  static const PackAssets::OfficialAssets originals(Data());
  if (originals.Contains(path)) {
    RE::BSResourceNiBinaryStream stream(path);
    if (!stream.stream || stream.stream->totalSize > 128 * 1024 * 1024)
      throw std::runtime_error("Asset too large to verify: " + path);
    std::vector<char> effective(stream.stream->totalSize);
    if (!effective.empty() &&
        !stream.read(effective.data(),
                     static_cast<std::uint32_t>(effective.size())))
      throw std::runtime_error("Could not verify asset: " + path);
    choice.gameFile = originals.Matches(path, effective);
  }
  checked.emplace(key, choice);
  return choice;
}

Result Discover(const Json &value) {
  Result result;
  result.discovered = true;
  std::set<std::string> neededPlugins, neededAssets;
  PackAssets::AssetUsers users;
  PackAssets::AssetDependencies dependencies;
  PackAssets::AssetUsers pluginUsers;
  PackAssets::AssetDependencies pluginDependencies;
  for (const auto &item : value["placeables"]) {
    const auto plugin = item["plugin"].get<std::string>();
    const auto itemName = item.value("name", "");
    const auto name = itemName.empty() ? std::string("Unnamed item") : itemName;
    pluginUsers[PackDocument::Lower(plugin)].insert(name);
    neededPlugins.insert(plugin);
    for (const auto &material : BuildCosts::Calculate(item)) {
      neededPlugins.insert(material.plugin);
      pluginUsers[PackDocument::Lower(material.plugin)].insert(name);
    }
    const auto identity = Identity(plugin, Local(item));
    const auto found = objectByIdentity.find(identity);
    if (found != objectByIdentity.end()) {
      const auto &entry = searchObjects[found->second];
      if (!entry.model.empty()) {
        const auto path = AssetPath(entry.model);
        neededAssets.insert(path);
        users[PackDocument::Lower(path)].insert(name);
      }
      if (!entry.winningPlugin.empty()) {
        neededPlugins.insert(entry.winningPlugin);
        pluginUsers[PackDocument::Lower(entry.winningPlugin)].insert(name);
      }
    }
  }
  std::vector<std::string> pluginQueue(neededPlugins.begin(),
                                       neededPlugins.end());
  for (std::size_t i = 0; i < pluginQueue.size(); ++i)
    if (const auto found = masters.find(PackDocument::Lower(pluginQueue[i]));
        found != masters.end())
      for (const auto &master : found->second) {
        pluginDependencies[PackDocument::Lower(pluginQueue[i])].push_back(
            PackDocument::Lower(master));
        if (neededPlugins.insert(master).second)
          pluginQueue.push_back(master);
      }
  PackAssets::PropagateUsers(pluginUsers, pluginDependencies);
  const std::set<std::string> baseFiles{"skyrim.esm",
                                        "update.esm",
                                        "dawnguard.esm",
                                        "hearthfires.esm",
                                        "dragonborn.esm",
                                        "skybuild.esp",
                                        "_resourcepack.esl",
                                        "ccbgssse001-fish.esm",
                                        "ccbgssse025-advdsgs.esm",
                                        "ccbgssse037-curios.esl",
                                        "ccqdrsse001-survivalmode.esl"};
  for (const auto &plugin : neededPlugins) {
    std::error_code error;
    const auto &names = pluginUsers[PackDocument::Lower(plugin)];
    result.plugins.push_back(
        {plugin,
         false,
         std::filesystem::is_regular_file(Data() / plugin, error),
         baseFiles.contains(PackDocument::Lower(plugin)),
         {names.begin(), names.end()}});
  }
  std::vector<std::string> assetQueue(neededAssets.begin(), neededAssets.end());
  for (std::size_t i = 0; i < assetQueue.size(); ++i) {
    const auto asset = assetQueue[i];
    const bool exists = Available(asset);
    result.assets.push_back(AssetChoice(asset));
    if (exists &&
        PackDocument::Lower(
            std::filesystem::path(asset).extension().string()) == ".nif")
      for (const auto &reference : MeshReferences(ReadMesh(asset))) {
        dependencies[PackDocument::Lower(asset)].push_back(
            PackDocument::Lower(reference));
        if (neededAssets.insert(reference).second)
          assetQueue.push_back(reference);
      }
  }
  PackAssets::PropagateUsers(users, dependencies);
  for (auto &asset : result.assets) {
    const auto &names = users[PackDocument::Lower(asset.path)];
    asset.usedBy.assign(names.begin(), names.end());
  }
  std::ranges::sort(result.assets, {}, &Choice::path);
  result.message.clear();
  return result;
}
std::optional<std::filesystem::path>
ChooseExportPath(const std::filesystem::path &pack, HWND owner) {
  const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                       COINIT_DISABLE_OLE1DDE);
  if (FAILED(initialized))
    throw std::runtime_error("Could not initialize the native Save As dialog.");
  struct COMScope {
    ~COMScope() { CoUninitialize(); }
  } scope;
  Microsoft::WRL::ComPtr<IFileSaveDialog> dialog;
  auto result = CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                 CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
  if (FAILED(result))
    throw std::runtime_error("Could not create the native Save As dialog.");
  FILEOPENDIALOGOPTIONS options{};
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                     FOS_OVERWRITEPROMPT | FOS_NOCHANGEDIR |
                     FOS_STRICTFILETYPES);
  const COMDLG_FILTERSPEC filters[]{{L"ZIP mod archive (*.zip)", L"*.zip"}};
  dialog->SetFileTypes(1, filters);
  dialog->SetDefaultExtension(L"zip");
  dialog->SetTitle(L"Export SkyBuild Pack");
  const auto filename = pack.stem().wstring() + L".zip";
  dialog->SetFileName(filename.c_str());
  const auto folder = Data() / "SkyBuild/exports";
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  if (!error) {
    Microsoft::WRL::ComPtr<IShellItem> initialFolder;
    if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr,
                                              IID_PPV_ARGS(&initialFolder))))
      dialog->SetDefaultFolder(initialFolder.Get());
  }
  result = dialog->Show(owner);
  if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
    return std::nullopt;
  if (FAILED(result))
    throw std::runtime_error("Native Save As dialog failed.");
  Microsoft::WRL::ComPtr<IShellItem> item;
  if (FAILED(dialog->GetResult(&item)))
    throw std::runtime_error("Could not read the chosen ZIP destination.");
  PWSTR filenameResult = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &filenameResult)))
    throw std::runtime_error("Choose a filesystem location for the ZIP.");
  std::filesystem::path path(filenameResult);
  CoTaskMemFree(filenameResult);
  return path;
}
Result Export(const std::filesystem::path &path, const Json &value,
              const std::vector<Choice> &selectedAssets,
              const std::vector<Choice> &selectedPlugins, HWND owner) {
  const auto destination = ChooseExportPath(path, owner);
  if (!destination) {
    Result cancelled;
    cancelled.message = "Export cancelled.";
    return cancelled;
  }
  WritePack(path, value);
  const auto final = *destination;
  auto temp = final;
  temp += L".tmp";
  try {
    PackZip archive(temp);
    const auto pack = path.stem().string();
    archive.Text("SkyBuild/packs/" + path.filename().string(), value.dump(2));
    for (const auto &plugin : selectedPlugins)
      if (plugin.include && !plugin.gameFile)
        archive.File(plugin.path, Data() / plugin.path);
    for (const auto &asset : selectedAssets)
      if (asset.include && !asset.gameFile && asset.available) {
        RE::BSResourceNiBinaryStream stream(asset.path);
        if (!stream.good() || !stream.stream)
          throw std::runtime_error("Missing selected asset: " + asset.path);
        archive.Add(asset.path, stream.stream->totalSize,
                    [&](char *buffer, std::uint32_t count) {
                      return stream.read(buffer, count);
                    });
      }
    archive.Close();
    if (!MoveFileExW(temp.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING))
      throw std::runtime_error("Could not publish export ZIP.");
    return {"Exported " + final.string(), std::nullopt, {}, {}, {}, true};
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    throw;
  }
}
void RefreshRuntime() {
  SKSE::GetTaskInterface()->AddTask([] {
    if (ThumbnailGenerator::GetProgress().running)
      return;
    (void)PlaceableRegistry::Load();
    ThumbnailGenerator::ResetPathCache();
    PlacementPreview::ReloadPlaceables();
    Settings::RefreshIconPacks();
    if (BuildingMode::IsActive())
      LoadPlaceableCarousel();
  });
}
void Load(const std::filesystem::path &path) {
  if (path != selectedPath) {
    itemFilter.clear();
    itemCategoryFilter.reset();
  }
  selectedPath = path;
  document = nullptr;
  message = "Loading pack...";
  exporting = false;
  assets.clear();
  ++assetCatalogRevision;
  plugins.clear();
  job = std::async(std::launch::async, [path] {
    std::ifstream input(path);
    if (!input)
      throw std::runtime_error("Cannot open pack.");
    Json value = Json::parse(input);
    (void)Packs::SchemaVersion(value);
    if (!value.contains("placeables") || !value["placeables"].is_array())
      throw std::runtime_error("Invalid item list.");
    for (auto &item : value["placeables"]) {
      if (!item.contains("category"))
        item["category"] = "Other";
      if (!item.contains("name"))
        item["name"] = "";
      const auto id = Local(item);
      if (id > 0xFFFFFF)
        item["formId"] = std::format(
            "0x{:06X}",
            id & ((id & 0xFF000000) == 0xFE000000 ? 0xFFF : 0xFFFFFF));
    }
    Packs::EnsureIdentities(value);
    PackDocument::EnsureCategories(value);
    return Result{"Pack loaded.", std::move(value)};
  });
}
// Pack documents and native actions remain owned by the game thread. The
// renderer receives immutable owned snapshots and queues actions back here.
std::atomic<bool> editorOpen{false};
bool stateDirty = true;
RE::FormID pickerDesiredForm = 0, pickerRequestedForm = 0, pickerReadyForm = 0,
           pickerFailedForm = 0;
std::uint64_t pickerGeneration = 0, pickerIconRevision = 0;
bool pickerPending = false;
RE::FormID hoverForm{};
std::string hoverPath;
std::uint64_t hoverGeneration{};
bool hoverPending{};
std::string lastFingerprint;
std::filesystem::path PickerPath() {
  return Data() / "SkyBuild/UI" / ThumbnailGenerator::PICKER_PREVIEW_PATH;
}
void InvalidateDraftPreview() {
  const auto generation = ThumbnailGenerator::GetProgress().generation;
  if ((pickerPending && generation == pickerGeneration) ||
      (hoverPending && generation == hoverGeneration))
    ThumbnailGenerator::Cancel();
  pickerPending = false;
  hoverPending = false;
  pickerReadyForm = pickerFailedForm = 0;
}
void ClearDraft() {
  CancelShell();
  InvalidateDraftPreview();
  pickedItem.reset();
  pickerDesiredForm = 0;
}
bool IsDoorItem(const Json &item) {
  const auto found =
      identities.find(Identity(item["plugin"].get<std::string>(), Local(item)));
  const auto object =
      found == identities.end()
          ? nullptr
          : RE::TESForm::LookupByID<RE::TESObjectDOOR>(found->second);
  return object != nullptr;
}
Json CostBounds(const Json &item) {
  const auto found = identities.find(
      Identity(item.at("plugin").get<std::string>(), Local(item)));
  const auto *object =
      found == identities.end()
          ? nullptr
          : RE::TESForm::LookupByID<RE::TESBoundObject>(found->second);
  if (!object)
    return item.value("costBounds", Json::array({64, 64, 64}));
  const auto &bounds = object->boundData;
  return Json::array({float(bounds.boundMax.x) - bounds.boundMin.x,
                      float(bounds.boundMax.y) - bounds.boundMin.y,
                      float(bounds.boundMax.z) - bounds.boundMin.z});
}
Json AutomaticValues(const PickerRecord &record, const Json &item,
                     std::string_view section) {
  Json values = Json::object();
  const bool all = section == "all";
  if (all || section == "details" || section == "category")
    values["category"] =
        Packs::SuggestCategory(record.recordType, record.editorID, record.name);
  if (all || section == "details" || section == "placement") {
    const auto preset = PackDocument::SnappingPreset(
        record.recordType == "Door"
            ? "wall"
            : PackDocument::SuggestPlacementPreset(
                  record.recordType == "Static", record.editorID));
    values.update(preset);
    values["doorway"] = record.recordType != "Door" &&
                        preset["type"] == "side" &&
                        item.value("doorway", false);
  }
  if (values.empty())
    throw std::runtime_error("Unknown automatic assignment section.");
  return values;
}
Json MaterialSnapshot(const Json &item) {
  auto values = BuildCosts::Encode(BuildCosts::Calculate(item));
  for (auto &material : values) {
    const auto found = materialByIdentity.find(
        Identity(material.at("plugin").get<std::string>(), Local(material)));
    if (found != materialByIdentity.end()) {
      const auto &record = materialObjects[found->second];
      material["name"] = record.name.empty() ? record.editorID : record.name;
      material["editorID"] = record.editorID;
    }
  }
  return values;
}
Json ItemSnapshot(const Json &item) {
  return {
      {"icon", PackDocument::IconRelative(
                   selectedPath.stem().string(),
                   item["plugin"].get<std::string>(), Local(item),
                   item.value("iconYaw", 0.F), item.value("iconLight", 1.F))
                   .generic_string()},
      {"itemId", item.value("itemId", "")},
      {"name", item.value("name", "")},
      {"plugin", item["plugin"]},
      {"formId", std::format("0x{:06X}", Local(item))},
      {"category", item.value("category", "")},
      {"type", item.value("type", "basic")},
      {"occupy", item.value("occupy", Json::array())},
      {"doorway", item.value("doorway", false)},
      {"alignToWall", item.value("alignToWall", false)},
      {"isDoor", IsDoorItem(item)},
      {"requiredMaterials", MaterialSnapshot(item)},
      {"iconYaw", item.value("iconYaw", 0.F)},
      {"iconLight", item.value("iconLight", 1.F)},
      {"rotationOffset",
       item.value("rotationOffset", Json{{"x", 0.F}, {"y", 0.F}, {"z", 0.F}})},
      {"freePlaceOffset",
       item.value("freePlaceOffset",
                  Json{{"x", 0.0F}, {"y", 0.0F}, {"z", 0.0F}})}};
}
std::vector<std::size_t> &FilteredItems() {
  static std::vector<std::size_t> indices;
  static std::uint64_t revision = ~0ULL;
  static std::string previous;
  if (revision != documentRevision || previous != itemFilter) {
    indices.clear();
    if (!document.is_null())
      for (std::size_t i = 0; i < document["placeables"].size(); ++i) {
        const auto &item = document["placeables"][i];
        if (PackDocument::MatchesFilter(item, itemFilter, itemCategoryFilter))
          indices.push_back(i);
      }
    revision = documentRevision;
    previous = itemFilter;
  }
  return indices;
}
void SendState() {
  if (!editorOpen.load())
    return;
  const auto progress = ThumbnailGenerator::GetProgress();
  if (!progress.running && progress.generation) {
    static std::uint64_t invalidated{};
    if (invalidated != progress.generation) {
      invalidated = progress.generation;
      if (!document.is_null())
        for (const auto &item : document["placeables"])
          Widgets::Invalidate(Widgets::Root() /
                              PackDocument::IconRelative(
                                  selectedPath.stem().string(),
                                  item["plugin"].get<std::string>(),
                                  Local(item), item.value("iconYaw", 0.F),
                                  item.value("iconLight", 1.F)));
    }
  }
  const auto &filtered = FilteredItems();
  Json items = Json::array();
  for (std::size_t i = 0; i < filtered.size(); ++i)
    items.push_back(
        {{"index", filtered[i]},
         {"position", i},
         {"item", ItemSnapshot(document["placeables"][filtered[i]])}});
  Json packNames = Json::array();
  for (const auto &path : packs)
    packNames.push_back(path.stem().string());
  auto choices = [](const std::vector<Choice> &list, std::size_t first,
                    std::size_t count) {
    Json output = Json::array();
    for (std::size_t i = first; i < std::min(list.size(), first + count); ++i)
      output.push_back({{"index", i},
                        {"path", list[i].path},
                        {"include", list[i].include},
                        {"available", list[i].available},
                        {"gameFile", list[i].gameFile},
                        {"usedBy", list[i].usedBy}});
    return output;
  };
  Json usedCategories = Json::array();
  if (!document.is_null())
    for (const auto &category : document["categories"])
      if (std::ranges::any_of(document["placeables"], [&](const auto &item) {
            return item.value("category", std::string("Other")) ==
                   category.get<std::string>();
          }))
        usedCategories.push_back(category);
  Json state{
      {"usedCategories", usedCategories},
      {"revision", documentRevision},
      {"packs", packNames},
      {"selected", selectedPath.empty() ? "" : selectedPath.stem().string()},
      {"loaded", !document.is_null()},
      {"dirty", dirty},
      {"busy", job.valid() || shellTarget.has_value() || !costBatch.empty() ||
                   !boundsBatch.empty()},
      {"shellStatus", shellStatus},
      {"shellAnalyzing", shellTarget.has_value()},
      {"shellReading", shellNeedsCapture || shellExtracting},
      {"shellEpoch", shellEpoch},
      {"message", message},
      {"plugins", objectPlugins},
      {"categories",
       document.is_null() ? Json::array() : document["categories"]},
      {"extendBasePack",
       document.is_null() ? false : document.value("extendBasePack", false)},
      {"items", items},
      {"itemCount", document.is_null() ? 0 : document["placeables"].size()},
      {"filteredCount", filtered.size()},
      {"categoryFilter",
       itemCategoryFilter ? Json(*itemCategoryFilter) : Json(nullptr)},
      {"draft", pickedItem ? ItemSnapshot(*pickedItem) : Json(nullptr)},
      {"draftReady", pickerDesiredForm &&
                         pickerReadyForm == pickerDesiredForm &&
                         !pickerPending},
      {"draftFailed",
       pickerDesiredForm && pickerFailedForm == pickerDesiredForm},
      {"draftIconRevision", pickerIconRevision},
      {"hoverPreview",
       {{"id", hoverPending ? 0U : hoverForm}, {"path", hoverPath}}},
      {"iconRevision", progress.running ? 0 : progress.generation},
      {"editorIDsAvailable", editorIDsAvailable},
      {"exportReady", exporting},
      {"exportPlugins", choices(plugins, 0, plugins.size())},
      {"assetCount", assets.size()},
      {"progress",
       {{"generation", progress.generation},
        {"total", progress.total},
        {"completed", progress.completed},
        {"failed", progress.failed},
        {"running", progress.running && !resourceCapture.has_value()},
        {"cancelling", progress.cancelling},
        {"cancelled", progress.cancelled},
        {"status", progress.status}}}};
  // Transfer the complete catalog only when files change, not on each progress
  // or checkbox update. Folder selection updates are compact prefix operations.
  if (sentAssetCatalogRevision != assetCatalogRevision) {
    state["exportAssets"] = choices(assets, 0, assets.size());
    sentAssetCatalogRevision = assetCatalogRevision;
  }
  if (!assetSelectionUpdate.is_null()) {
    state["assetSelectionUpdate"] = assetSelectionUpdate;
    assetSelectionUpdate = nullptr;
  }
  View::Publish(std::move(state));
  stateDirty = false;
}
void StartPackSave() {
  if (ThumbnailGenerator::GetProgress().running)
    throw std::runtime_error("Wait for icon generation to finish.");
  const auto validation = PackDocument::Validate(document);
  if (!validation.empty())
    throw std::runtime_error(validation);
  const auto path = selectedPath;
  const auto value = document;
  job = std::async(std::launch::async, [path, value] {
    WritePack(path, value);
    Result result;
    result.reload = true;
    result.message = "Pack saved and applied.";
    return result;
  });
}
void ApplyJob() {
  if (!job.valid() ||
      job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    return;
  try {
    auto result = job.get();
    message = result.message;
    if (!result.packs.empty())
      packs = std::move(result.packs);
    if (result.document) {
      const bool migrated = Packs::MigratePack(*result.document, [](auto &item) {
        if (item.value("objectSize", std::string{}) == "Auto")
          item["costBounds"] = CostBounds(item);
      });
      const auto validation = PackDocument::Validate(*result.document);
      if (!validation.empty())
        throw std::runtime_error(validation);
      document = std::move(*result.document);
      dirty = migrated;
      ++documentRevision;
      RememberPack();
    }
    if (result.addedItem) {
      exporting = false;
      document["placeables"].push_back(std::move(*result.addedItem));
      ClearDraft();
      dirty = true;
      ++documentRevision;
    }
    if (!result.discovered && !result.assets.empty()) {
      assets.insert(assets.end(), result.assets.begin(), result.assets.end());
      ++assetCatalogRevision;
    }
    if (result.discovered) {
      assets = std::move(result.assets);
      plugins = std::move(result.plugins);
      exporting = true;
      ++assetCatalogRevision;
    }
    if (result.reload) {
      dirty = false;
      RefreshRuntime();
    }
    if (restorePackPending) {
      restorePackPending = false;
      const auto found = std::ranges::find_if(packs, [](const auto &path) {
        return path.stem().string() == lastPackName;
      });
      if (found != packs.end() && document.is_null())
        Load(*found);
    }
  } catch (const std::exception &error) {
    message = error.what();
    if (closeSavePending) {
      closeSavePending = false;
      View::SetOpen(true);
      editorOpen.store(true);
      RefreshPresentation();
    }
  }
  stateDirty = true;
}
void UpdatePreview() {
  if (pickerPending) {
    const auto progress = ThumbnailGenerator::GetProgress();
    if (progress.generation == pickerGeneration && progress.running)
      return;
    if (progress.generation == pickerGeneration && !progress.failed &&
        !progress.cancelled && progress.completed == progress.total) {
      pickerReadyForm = pickerRequestedForm;
      ++pickerIconRevision;
      Widgets::Invalidate(PickerPath(), true);
    } else
      pickerFailedForm = pickerRequestedForm;
    pickerPending = false;
    stateDirty = true;
  }
  if (shellTarget || !editorOpen.load() || !pickedItem || !pickerDesiredForm ||
      job.valid() || pickerPending || pickerReadyForm == pickerDesiredForm ||
      pickerFailedForm == pickerDesiredForm ||
      ThumbnailGenerator::GetProgress().running)
    return;
  const auto before = ThumbnailGenerator::GetProgress().generation;
  ThumbnailGenerator::QueueEditorIcon(
      pickerDesiredForm, std::string(ThumbnailGenerator::PICKER_PREVIEW_PATH),
      PlacementPreview::PlaceableType::kBasic,
      pickedItem->value("iconYaw", 0.F), pickedItem->value("iconLight", 1.F));
  const auto after = ThumbnailGenerator::GetProgress().generation;
  if (after > before) {
    pickerRequestedForm = pickerDesiredForm;
    pickerReadyForm = 0;
    pickerGeneration = after;
    pickerPending = true;
    stateDirty = true;
  }
}
Json &TargetItem(const Json &command) {
  if (command.value("draft", false)) {
    if (!pickedItem)
      throw std::runtime_error("Select an item first.");
    if (!PackDocument::SameItemSource(*pickedItem, command.at("source")))
      throw std::runtime_error("The selected record changed. Try again.");
    return *pickedItem;
  }
  const auto index = PackDocument::FindItemIndex(
      document, command.at("source"),
      command.contains("index")
          ? std::optional{command["index"].get<std::size_t>()}
          : std::nullopt);
  if (!index)
    throw std::runtime_error("This item is no longer in the pack.");
  return document.at("placeables").at(*index);
}
void QueueItemIcon(const Json &item) {
  const auto found =
      identities.find(Identity(item["plugin"].get<std::string>(), Local(item)));
  if (found == identities.end())
    throw std::runtime_error("This record is not loaded.");
  const auto output =
      PackDocument::IconRelative(
          selectedPath.stem().string(), item["plugin"].get<std::string>(),
          Local(item), item.value("iconYaw", 0.F), item.value("iconLight", 1.F))
          .generic_string();
  ThumbnailGenerator::QueueEditorIcon(
      found->second, output, PlacementPreview::PlaceableType::kBasic,
      item.value("iconYaw", 0.F), item.value("iconLight", 1.F));
}
void Action(const Json &command);
void UpdateShell() {
  if (shellWorker.valid() && shellWorker.wait_for(std::chrono::seconds(0)) ==
                                 std::future_status::ready) {
    try {
      const auto result = shellWorker.get();
      if (shellTarget && shellWorkerEpoch == shellEpoch &&
          shellTarget->value("pack", std::string{}) ==
              selectedPath.stem().string()) {
        Json textures = Json::array();
        const auto &item = TargetItem(*shellTarget);
        const auto found = objectByIdentity.find(
            Identity(item.at("plugin").get<std::string>(), Local(item)));
        const auto editor = found == objectByIdentity.end()
                                ? std::string{}
                                : searchObjects[found->second].editorID;
        for (const auto &[path, footprint] : result.textures) {
          const auto guesses = BuildCosts::SuggestCostMaterials(path);
          const auto guess =
              path.empty() ? BuildCosts::SuggestCostMaterials(editor).front()
                           : guesses.front();
          textures.push_back({{"texture", path},
                              {"footprint", footprint},
                              {"material", guess}});
        }
        auto edit = *shellTarget;
        edit["action"] = "edit";
        edit["values"] = {
            {"requiredMaterials",
             BuildCosts::Encode(BuildCosts::CalculateComponentCosts(
                 {{"woodBudget", result.woodBudget}, {"textures", textures}}))}};

        Action(edit);
        shellTarget.reset();
        shellStatus.clear();
      }
    } catch (const std::exception &error) {
      if (shellWorkerEpoch == shellEpoch && shellTarget) {
        shellStatus = error.what();
        message = shellStatus;
        shellTarget.reset();
      }
    }
    stateDirty = true;
  }
  if (!shellTarget)
    return;
  if (shellExtracting) {
    const auto mesh = ThumbnailGenerator::TakeMaterialMesh();
    const auto progress = ThumbnailGenerator::GetProgress();
    if (mesh && mesh->generation == shellGeneration) {
      shellExtracting = false;
      if (!mesh->error.empty()) {
        shellStatus = mesh->error;
        message = shellStatus;
        shellTarget.reset();
        stateDirty = true;
        return;
      }
      shellCancel = std::make_shared<std::atomic_bool>(false);
      shellWorkerEpoch = shellEpoch;
      const auto cancel = shellCancel;
      shellStatus = "Measuring projected material footprints...";
      shellWorker = std::async(std::launch::async, [mesh, cancel] {
        return MaterialShell::AnalyzeFootprint(mesh->mesh, *cancel);
      });
      stateDirty = true;
    } else if (progress.generation != shellGeneration || !progress.running) {
      shellExtracting = false;
      shellStatus =
          "Could not read the mesh's triangle surfaces. Try Auto again.";
      message = shellStatus;
      shellTarget.reset();
      stateDirty = true;
    }
    return;
  }
  if (!shellNeedsCapture || shellWorker.valid() || job.valid() ||
      ThumbnailGenerator::GetProgress().running)
    return;
  const auto &item = TargetItem(*shellTarget);
  const auto found = identities.find(
      Identity(item.at("plugin").get<std::string>(), Local(item)));
  if (found == identities.end()) {
    shellStatus = "Source object unavailable.";
    message = shellStatus;
    shellTarget.reset();
    stateDirty = true;
    return;
  }
  const auto before = ThumbnailGenerator::GetProgress().generation;
  ThumbnailGenerator::QueueMaterialAnalysis(found->second);
  const auto after = ThumbnailGenerator::GetProgress().generation;
  if (after > before) {
    shellNeedsCapture = false;
    shellExtracting = true;
    shellGeneration = after;
    shellStatus = "Reading triangle surfaces and textures...";
    stateDirty = true;
  }
}
void Action(const Json &command) {
  if (command.value("action", "") == "resourceIcon") {
    QueueResourceIcon(command.at("plugin").get<std::string>(), Local(command),
                      command.value("temporary", false));
    return;
  }

  const auto action = command.value("action", "");
  if (!boundsBatch.empty() && action != "close" && action != "poll" &&
      action != "tableIcon")
    throw std::runtime_error("Wait for bounds recalculation to finish.");
  if (dirty && (action == "createPack" || action == "loadPack"))
    throw std::runtime_error(
        "Save or revert the current pack before switching packs.");
  if (action != "close" && action != "createPack" && action != "loadPack" &&
      command.value("pack", "") != selectedPath.stem().string())
    throw std::runtime_error("The selected pack changed. Try again.");
  if (action == "cancelCostAnalysis") {
    // Ignore a click from an older item/job after a new analysis has started.
    if (command.value("epoch", std::uint64_t{}) == shellEpoch && shellTarget) {
      costBatch.clear();
      CancelShell();
      stateDirty = true;
    }
    return;
  }
  if (action == "tableIcon") {
    const auto plugin = command.at("plugin").get<std::string>();
    const auto local = Local(command);
    const auto found = objectByIdentity.find(Identity(plugin, local));
    if (found == objectByIdentity.end())
      return;
    const auto path =
        PackDocument::IconRelative(command.value("picker", false)
                                       ? ".hover"
                                       : selectedPath.stem().string(),
                                   plugin, local, command.value("iconYaw", 0.F),
                                   command.value("iconLight", 1.F))
            .generic_string();
    ThumbnailGenerator::RegisterAppearance(searchObjects[found->second].id,
                                           path, command.value("iconYaw", 0.F),
                                           command.value("iconLight", 1.F));
    if (requestedResourceIcons.insert(path).second &&
        !ThumbnailGenerator::HasThumbnail(path))
      resourceIcons.emplace_back(searchObjects[found->second].id, path);
    return;
  }
  if (action == "hoverPreview") {
    const auto id = command.at("id").get<RE::FormID>();
    if (hoverPending || pickerPending || job.valid() ||
        ThumbnailGenerator::GetProgress().running)
      return;
    const auto found = std::ranges::find_if(
        searchObjects, [id](const auto &entry) { return entry.id == id; });
    if (found == searchObjects.end())
      return;
    hoverForm = id;
    hoverPath = ThumbnailGenerator::GetRelativePath(id).value_or(
        PackDocument::IconRelative(".hover", found->plugin, found->local)
            .generic_string());
    if (!ThumbnailGenerator::HasThumbnail(hoverPath)) {
      const auto before = ThumbnailGenerator::GetProgress().generation;
      ThumbnailGenerator::QueueRegeneration(
          std::vector<ThumbnailGenerator::IconRequest>{{id, hoverPath}});
      hoverGeneration = ThumbnailGenerator::GetProgress().generation;
      hoverPending = hoverGeneration > before;
    }
    stateDirty = true;
    Update();
    return;
  }
  if (action == "poll") {
    Update();
    return;
  }
  if (action == "close") {
    Close();
    return;
  }
  if (action == "cancelGeneration") {
    ThumbnailGenerator::Cancel();
    stateDirty = true;
    Update();
    return;
  }
  if (action == "materialSearch") {
    const auto query = PackDocument::Lower(command.value("query", ""));
    auto result = Json::array();
    const auto append = [&](const MaterialRecord &entry) {
      if (entry.selectable && !entry.name.empty())
        result.push_back({{"name", entry.name},
                          {"plugin", entry.plugin},
                          {"formId", std::format("0x{:06X}", entry.local)}});
    };
    std::unordered_set<RE::FormID> suggested;
    for (const auto &material : BuildCosts::StandardMaterials) {
      const auto found =
          materialByIdentity.find(Identity(material.plugin, material.local));
      if (found == materialByIdentity.end())
        continue;
      const auto &entry = materialObjects[found->second];
      suggested.insert(entry.id);
      if (entry.nameSearch.find(query) != std::string::npos)
        append(entry);
    }
    if (!query.empty()) {
      for (const auto &entry : materialObjects) {
        if (!suggested.contains(entry.id) &&
            entry.nameSearch.find(query) != std::string::npos)
          append(entry);
        if (result.size() == 200)
          break;
      }
    }
    View::MaterialResults({{"requestId", command.value("requestId", 0ULL)},
                           {"results", std::move(result)}});
    return;
  }
  if (action == "search") {
    const auto query = PackDocument::Lower(command.value("query", "")),
               plugin = PackDocument::Lower(command.value("plugin", ""));
    const auto recordType = command.value("recordType", "");
    std::unordered_set<std::string> packed;
    if (!document.is_null())
      for (const auto &item : document["placeables"])
        packed.emplace(
            Identity(item["plugin"].get<std::string>(), Local(item)));
    Json results = Json::array();
    for (const auto &entry : searchObjects) {
      if ((!recordType.empty() && entry.recordType != recordType) ||
          !PackDocument::MatchesRecordSearch(query, entry.search, entry.name) ||
          (!plugin.empty() && entry.pluginSearch != plugin))
        continue;
      const bool inPack = packed.contains(Identity(entry.plugin, entry.local));
      results.push_back({{"recordType", entry.recordType},
                         {"inPack", inPack},
                         {"id", entry.id},
                         {"name", entry.name},
                         {"editorID", entry.editorID},
                         {"plugin", entry.plugin},
                         {"local", std::format("0x{:06X}", entry.local)}});
    }
    View::SearchResults(Json{{"requestId", command.value("requestId", 0)},
                             {"results", std::move(results)}});
    return;
  }
  if (job.valid())
    throw std::runtime_error("Wait for the current operation to finish.");
  if (action == "selectWorldItem") {
    if (document.is_null())
      throw std::runtime_error("Select a pack first.");
    if (!BuildingMode::IsHammerReady())
      throw std::runtime_error(
          "Equip the Construction Hammer to select an object.");
    ClearDraft();
    selectionWasTerrain = Terrain::Enabled();
    selectionReturnTool = BuildingMode::GetTool();
    Terrain::SetEnabled(false);
    BuildingMode::SetEnabled(true);
    BuildingMode::SelectTool(BuildingMode::InteractionMode::kPicker);
    selectingWorldItem = true;
    closeSavePending = false;
    editorOpen.store(false);
    View::SetOpen(false);
    RefreshPresentation();
    return;
  } else if (action == "deletePack") {
    if (document.is_null())
      throw std::runtime_error("Select a pack first.");
    if (ThumbnailGenerator::GetProgress().running)
      throw std::runtime_error(
          "Wait for icon generation to finish before deleting the pack.");
    const auto deleted = selectedPath.stem().string();
    PackDocument::DeletePackFile(Data() / "SkyBuild/packs", selectedPath,
                                 command.value("confirmation", ""));
    std::erase(packs, selectedPath);
    selectedPath.clear();
    RememberPack();
    document = nullptr;
    dirty = closeSavePending = exporting = false;
    ClearDraft();
    assets.clear();
    plugins.clear();
    ++assetCatalogRevision;
    itemFilter.clear();
    itemCategoryFilter.reset();
    ++documentRevision;
    message = "Deleted pack " + deleted + ".";
    RefreshRuntime();
  } else if (action == "createPack") {
    const auto name = command.at("name").get<std::string>();
    if (dirty)
      throw std::runtime_error("Save or discard the current pack first.");
    if (!PackDocument::ValidName(name))
      throw std::runtime_error("Use a valid pack name.");
    selectedPath = Data() / "SkyBuild/packs" / (name + ".json");
    exporting = false;
    assets.clear();
    plugins.clear();
    ++assetCatalogRevision;
    itemFilter.clear();
    itemCategoryFilter.reset();
    document = nullptr;
    ClearDraft();
    ++documentRevision;
    const auto path = selectedPath;
    job = std::async(std::launch::async, [path] {
      auto value = PackDocument::Create();
      std::filesystem::create_directories(path.parent_path());
      WritePack(path, value, true);
      Result result;
      result.document = value;
      result.packs = PackPaths();
      result.reload = true;
      result.message = "Pack created.";
      return result;
    });
  } else if (action == "loadPack") {
    if (dirty)
      throw std::runtime_error("Save or discard the current pack first.");
    const auto name = command.at("name").get<std::string>();
    const auto found = std::ranges::find_if(
        packs, [&](const auto &path) { return path.stem().string() == name; });
    if (found == packs.end())
      throw std::runtime_error("Pack not found.");
    ClearDraft();
    ++documentRevision;
    Load(*found);
  } else if (action == "save") {
    StartPackSave();
  } else if (action == "discard") {
    ClearDraft();
    ++documentRevision;
    Load(selectedPath);
  } else if (action == "extend") {
    document["extendBasePack"] = command.at("value").get<bool>();
    dirty = true;
    ++documentRevision;
  } else if (action == "itemFilter") {
    itemFilter = command.value("value", "");
    if (command.contains("category")) {
      if (command["category"].is_null())
        itemCategoryFilter.reset();
      else {
        const auto category = command["category"].get<std::string>();
        if (!category.empty() &&
            std::find(document["categories"].begin(),
                      document["categories"].end(),
                      Json(category)) == document["categories"].end())
          throw std::runtime_error("Category not found.");
        itemCategoryFilter = category;
      }
    }
    ++documentRevision;
  } else if (action == "pick") {
    const auto id = command.at("id").get<RE::FormID>();
    const auto found = std::ranges::find_if(
        searchObjects, [id](const auto &entry) { return entry.id == id; });
    if (found == searchObjects.end())
      throw std::runtime_error("Object not found.");
    for (const auto &item : document["placeables"])
      if (Identity(item["plugin"].get<std::string>(), Local(item)) ==
          Identity(found->plugin, found->local))
        throw std::runtime_error("This record is already in the pack.");
    CancelShell();
    InvalidateDraftPreview();
    pickedItem =
        Json{{"itemId", Packs::Id::New().String()},
             {"name", found->name.empty() ? found->editorID : found->name},
             {"plugin", found->plugin},
             {"formId", std::format("0x{:06X}", found->local)},
             {"alignToWall", false},
             {"freePlaceOffset", {{"x", 0.0F}, {"y", 0.0F}, {"z", 0.0F}}}};
    pickedItem->update(AutomaticValues(*found, *pickedItem, "all"));
    Models::EnsureVertexBounds(*pickedItem,
                               RE::TESForm::LookupByID<RE::TESBoundObject>(id));
    (*pickedItem)["requiredMaterials"] = Json::array();
    RequestShell(Json{{"pack", selectedPath.stem().string()},
                      {"draft", true},
                      {"source",
                       {{"plugin", (*pickedItem)["plugin"]},
                        {"formId", (*pickedItem)["formId"]}}}});

    pickerDesiredForm = id;
    pickerReadyForm = pickerFailedForm = 0;
    ++documentRevision;
    message.clear();
  } else if (action == "clearDraft") {
    ClearDraft();
    ++documentRevision;
  } else if (action == "recalculateBounds" ||
             action == "recalculateAllBounds") {
    if (job.valid() || shellTarget || !costBatch.empty() ||
        !boundsBatch.empty())
      throw std::runtime_error("Wait for the current operation to finish.");
    boundsFailed = 0;
    refreshedBoundsModels.clear();
    if (action == "recalculateBounds") {
      (void)TargetItem(command);
      boundsBatch.push_back(command);
    } else {
      for (const auto &item : document["placeables"])
        boundsBatch.push_back(
            {{"source",
              {{"plugin", item["plugin"]}, {"formId", item["formId"]}}}});
    }
    boundsTotal = boundsBatch.size();
    message = boundsTotal
                  ? std::format("Recalculating bounds: 0 / {}", boundsTotal)
                  : "No items to recalculate.";
  } else if (action == "recalculateCosts") {
    costBatch.clear();
    for (const auto &item : document["placeables"])
      costBatch.push_back(
          {{"pack", selectedPath.stem().string()},
           {"source", {{"plugin", item["plugin"]}, {"formId", item["formId"]}}},
           {"section", "costs"}});
  } else if (action == "autoAssign") {
    const auto &item = TargetItem(command);
    if (command.at("section") == "materials" ||
        command.at("section") == "costs") {
      RequestShell(command);
      stateDirty = true;
      return;
    }
    const auto found = objectByIdentity.find(
        Identity(item.at("plugin").get<std::string>(), Local(item)));
    if (found == objectByIdentity.end())
      throw std::runtime_error("This item's source record is unavailable.");
    auto edit = command;
    edit["action"] = "edit";
    edit["values"] = AutomaticValues(searchObjects[found->second], item,
                                     command.at("section").get<std::string>());
    Action(edit);
  } else if (action == "edit") {
    auto &item = TargetItem(command);
    auto candidate = item;
    const auto &values = command.at("values");
    for (const auto field :
         {"name", "category", "type", "occupy", "freePlaceOffset",
          "rotationOffset", "doorway", "alignToWall", "requiredMaterials"})
      if (values.contains(field))
        candidate[field] = values[field];
    if (values.contains("requiredMaterials")) {
      const auto costs = BuildCosts::Decode(candidate["requiredMaterials"]);
      for (const auto &cost : costs)
        if (!materialByIdentity.contains(Identity(cost.plugin, cost.local)))
          throw std::runtime_error(
              "Required material is not an available inventory item.");
      candidate["requiredMaterials"] = BuildCosts::Encode(costs);
    }
    for (const auto &cost : BuildCosts::Calculate(candidate))
      if (!materialByIdentity.contains(Identity(cost.plugin, cost.local)))
        throw std::runtime_error(
            "A selected building material is unavailable.");
    if (IsDoorItem(candidate)) {
      candidate["type"] = "side";
      candidate["occupy"] = Json::array();
      candidate["doorway"] = false;
    }
    const auto validation =
        PackDocument::Validate(Json{{"categories", document["categories"]},
                                    {"placeables", Json::array({candidate})}});
    if (!validation.empty())
      throw std::runtime_error(validation);
    const bool nameChanged =
        item.value("name", "") != candidate.value("name", "");
    item = std::move(candidate);
    if (!command.value("draft", false)) {
      if (nameChanged)
        exporting = false;
      dirty = true;
    }
    ++documentRevision;
  } else if (action == "itemMove") {
    const auto &source = command.at("source");
    Json before = command.value("before", Json(nullptr));
    if (command.contains("direction")) {
      const auto index = PackDocument::FindItemIndex(document, source);
      if (!index)
        throw std::runtime_error("This item is no longer in the pack.");
      const auto &visible = FilteredItems();
      const auto found = std::find(visible.begin(), visible.end(), *index);
      if (found == visible.end())
        return;
      const auto position = static_cast<std::size_t>(found - visible.begin());
      if (command.at("direction").get<int>() < 0) {
        if (!position)
          return;
        before = document["placeables"][visible[position - 1]];
      } else {
        if (position + 1 >= visible.size())
          return;
        if (position + 2 < visible.size())
          before = document["placeables"][visible[position + 2]];
      }
    }
    if (PackDocument::MoveItem(document, source, before)) {
      exporting = false;
      dirty = true;
      ++documentRevision;
    }
  } else if (action == "removeItem") {
    exporting = false;
    const auto index =
        PackDocument::FindItemIndex(document, command.at("source"));
    if (!index)
      throw std::runtime_error("This item is no longer in the pack.");
    auto &items = document.at("placeables");
    items.erase(items.begin() + *index);
    dirty = true;
    ++documentRevision;
  } else if (action == "categoryCreate" || action == "categoryRename") {
    const auto name = command.at("name").get<std::string>();
    if (name.empty() || name.front() == ' ' || name.back() == ' ' ||
        PackDocument::Lower(name) == "uncategorized")
      throw std::runtime_error("Choose a unique category name.");
    const auto old = command.value("old", "");
    for (const auto &entry : document["categories"])
      if (entry.get<std::string>() != old &&
          PackDocument::Lower(entry.get<std::string>()) ==
              PackDocument::Lower(name))
        throw std::runtime_error("Category already exists.");
    if (action == "categoryRename") {
      if (Packs::IsDefaultCategory(old))
        throw std::runtime_error("Default categories cannot be renamed.");
      auto &list = document["categories"];
      const auto found = std::find(list.begin(), list.end(), Json(old));
      if (found == list.end())
        throw std::runtime_error("Category not found.");
      *found = name;
      if (itemCategoryFilter == old)
        itemCategoryFilter = name;
      for (auto &item : document["placeables"])
        if (PackDocument::Lower(item.value("category", "")) ==
            PackDocument::Lower(old))
          item["category"] = name;
      if (pickedItem && PackDocument::Lower(pickedItem->value(
                            "category", "")) == PackDocument::Lower(old))
        (*pickedItem)["category"] = name;
    } else {
      if (command.contains("assign"))
        (void)TargetItem(command["assign"]);
      document["categories"].push_back(name);
      Packs::EnsureDefaultCategories(document["categories"]);
      if (command.contains("assign"))
        TargetItem(command["assign"])["category"] = name;
    }
    dirty = true;
    ++documentRevision;
  } else if (action == "categoryMove") {
    PackDocument::MoveCategory(document, command.at("name").get<std::string>(),
                               command.value("before", ""));
    dirty = true;
    ++documentRevision;
  } else if (action == "categoryDelete") {
    const auto removed = command.at("old").get<std::string>();
    const auto found = std::find(document["categories"].begin(),
                                 document["categories"].end(), Json(removed));
    if (found == document["categories"].end())
      throw std::runtime_error("Category no longer exists.");
    const auto index =
        static_cast<std::size_t>(found - document["categories"].begin());
    if (itemCategoryFilter == removed)
      itemCategoryFilter.reset();
    if (pickedItem && PackDocument::Lower(pickedItem->value("category", "")) ==
                          PackDocument::Lower(removed))
      (*pickedItem)["category"] = "Other";
    PackDocument::RemoveCategory(document, index);
    dirty = true;
    ++documentRevision;
  } else if (action == "adjustIcon") {
    auto &item = TargetItem(command);
    if (!command.value("draft", false) &&
        ThumbnailGenerator::GetProgress().running)
      throw std::runtime_error("Wait for the icon to finish generating.");
    const auto control = command.at("control").get<std::string>();
    const int step = command.at("step").get<int>();
    if (step < 0 || step > 3)
      throw std::runtime_error("Invalid icon adjustment step.");
    if (control == "rotation")
      item["iconYaw"] = step * 90.F;
    else if (control == "light")
      item["iconLight"] = (step + 1) * .5F;
    else
      throw std::runtime_error("Unknown icon control.");
    if (command.value("draft", false)) {
      InvalidateDraftPreview();
    } else {
      dirty = true;
      ++documentRevision;
      QueueItemIcon(item);
    }
  } else if (action == "regenerate") {
    if (command.value("draft", false)) {
      InvalidateDraftPreview();
    } else
      QueueItemIcon(TargetItem(command));
  } else if (action == "add") {
    if (!pickedItem ||
        !PackDocument::SameItemSource(*pickedItem, command.at("source")))
      throw std::runtime_error("The selected record changed. Try again.");
    if ((pickerReadyForm != pickerDesiredForm &&
         pickerFailedForm != pickerDesiredForm) ||
        pickerPending || ThumbnailGenerator::GetProgress().running)
      throw std::runtime_error(
          "Wait for the selected icon to finish generating.");
    const auto item = *pickedItem;
    const auto validation =
        PackDocument::Validate(Json{{"categories", document["categories"]},
                                    {"placeables", Json::array({item})}});
    if (!validation.empty())
      throw std::runtime_error(validation);
    const auto destination =
        Data() / "SkyBuild/UI" /
        PackDocument::IconRelative(selectedPath.stem().string(),
                                   item["plugin"].get<std::string>(),
                                   Local(item), item.value("iconYaw", 0.F),
                                   item.value("iconLight", 1.F));
    const bool hasPreview = pickerReadyForm == pickerDesiredForm;
    job = std::async(std::launch::async, [item, destination, hasPreview] {
      if (hasPreview) {
        if (!ThumbnailGenerator::SaveTemporary(
                ThumbnailGenerator::PICKER_PREVIEW_PATH, destination))
          throw std::runtime_error("Could not save the icon.");
      }
      Result result;
      result.addedItem = item;
      result.message = "Item added. Save Changes to apply your changes.";
      return result;
    });
  } else if (action == "prepareExport") {
    const auto validation = PackDocument::Validate(document);
    if (!validation.empty())
      throw std::runtime_error(validation);
    if (ThumbnailGenerator::GetProgress().running)
      throw std::runtime_error("Wait for icon generation to finish.");
    exporting = false;
    assets.clear();
    ++assetCatalogRevision;
    plugins.clear();
    const auto value = document;
    job = std::async(std::launch::async, [value] { return Discover(value); });
    message = "Checking models, textures, and plugins...";
  } else if (action == "include") {
    auto &list = command.value("kind", "") == "plugins" ? plugins : assets;
    const auto expected =
        PackDocument::Lower(command.at("path").get<std::string>());
    const auto found = std::ranges::find_if(list, [&](const auto &entry) {
      return PackDocument::Lower(entry.path) == expected;
    });
    if (found == list.end())
      throw std::runtime_error("Export entry no longer exists.");
    const auto index = static_cast<std::size_t>(found - list.begin());
    auto &choice = *found;
    if (choice.available && !choice.gameFile)
      choice.include = command.at("value").get<bool>();
    if (command.value("kind", "") == "assets")
      assetSelectionUpdate = {{"index", index}, {"value", choice.include}};
  } else if (action == "selectAssetFolder") {
    const auto prefix =
        PackDocument::Lower(AssetPath(command.at("prefix").get<std::string>()));
    if (!prefix.ends_with('/'))
      throw std::runtime_error("Choose a folder path.");
    const auto value = command.at("value").get<bool>();
    for (auto &choice : assets)
      if (choice.available && !choice.gameFile &&
          PackDocument::Lower(choice.path).starts_with(prefix))
        choice.include = value;
    assetSelectionUpdate = {{"prefix", prefix}, {"value", value}};
  } else if (action == "selectAssets") {
    for (auto &choice : assets)
      choice.include = choice.available && !choice.gameFile &&
                       command.at("value").get<bool>();
    assetSelectionUpdate = {{"prefix", ""}, {"value", command.at("value")}};
  } else if (action == "addAsset") {
    const auto path = AssetPath(command.at("path").get<std::string>());
    if (std::ranges::none_of(assets, [&](const auto &entry) {
          return PackDocument::Lower(entry.path) == PackDocument::Lower(path);
        }))
      job = std::async(std::launch::async, [path] {
        Result result;
        result.assets.push_back(AssetChoice(path));
        return result;
      });
  } else if (action == "export") {
    if (!exporting)
      throw std::runtime_error("Wait for asset discovery.");
    const auto path = selectedPath;
    const auto value = document;
    const auto selectedAssets = assets, selectedPlugins = plugins;
    const auto owner = GetForegroundWindow();
    job = std::async(std::launch::async, [=] {
      return Export(path, value, selectedAssets, selectedPlugins, owner);
    });
    message = "Choose where to save the ZIP...";
  } else
    throw std::runtime_error("Unknown editor action.");
  stateDirty = true;
  Update();
}
void ReceiveAction(const Json &command) {
  SKSE::GetTaskInterface()->AddTask([command] {
    if (!editorOpen.load())
      return;
    try {
      Action(command);
    } catch (const std::exception &error) {
      message = error.what();
      stateDirty = true;
      Update();
    }
  });
}
} // namespace
void Initialize() {
  try {
    std::ifstream input(Data() / "SkyBuild/PackEditorState.json");
    if (input)
      lastPackName = Json::parse(input).value("lastPack", "");
  } catch (const std::exception &error) {
    spdlog::warn("Could not read Pack Editor selection: {}", error.what());
  }
  View::Initialize(ReceiveAction, [] {
    // Jobs continue on the game thread while the workspace menu pauses player
    // updates.
    static std::atomic_bool pending{};
    if (!pending.exchange(true))
      SKSE::GetTaskInterface()->AddTask([] {
        ThumbnailGenerator::Update();
        Update();
        pending.store(false);
      });
  });
  const auto handler = RE::TESDataHandler::GetSingleton();
  if (!handler)
    return;
  using EditorIDLookup = const char *(*)(std::uint32_t);
  const auto tweaks = GetModuleHandleW(L"po3_Tweaks.dll");
  const auto lookup = tweaks ? reinterpret_cast<EditorIDLookup>(
                                   GetProcAddress(tweaks, "GetFormEditorID"))
                             : nullptr;
  const auto collect = [&](const auto &objects, const char *recordType) {
    for (const auto form : objects) {
      // Form arrays contain TESForm addresses. In particular MovableStatic's
      // TESObjectSTAT base is not at offset zero: resolve through RTTI instead
      // of reinterpreting the array as derived pointers.
      const auto object =
          form ? form->template As<RE::TESBoundObject>() : nullptr;
      if (!object || object->IsDeleted())
        continue;
      const auto file = object->GetFile(0);
      if (!file)
        continue;
      PickerRecord entry{object->GetFormID(),
                         object->GetLocalFormID(),
                         object->GetName() ? object->GetName() : "",
                         object->GetFormEditorID() ? object->GetFormEditorID()
                                                   : "",
                         std::string(file->GetFilename()),
                         {},
                         {}};
      if (const auto model = object->As<RE::TESModel>())
        if (const auto path = model->GetModel())
          entry.model = path;
      // Statics generally do not retain editor IDs in the game's TESForm.
      // Use the optional Tweaks cache when the normal getter is empty.
      if (entry.editorID.empty() && lookup)
        if (const auto editorID = lookup(entry.id))
          entry.editorID = editorID;
      entry.recordType = recordType;
      entry.search = PackDocument::Lower(entry.editorID);
      entry.pluginSearch = PackDocument::Lower(entry.plugin);
      if (const auto winning = object->GetFile())
        entry.winningPlugin = std::string(winning->GetFilename());
      identities.emplace(Identity(entry.plugin, entry.local), entry.id);
      // GetModel() is the record's stored model path: no NIF load or disk
      // probe. Keep identity metadata even for forms excluded from the add-item
      // picker.
      if (!entry.model.empty())
        searchObjects.push_back(std::move(entry));
      std::vector<const RE::TESFile *> files{file, object->GetFile()};
      for (std::size_t i = 0; i < files.size(); ++i) {
        const auto current = files[i];
        if (!current)
          continue;
        const auto key =
            PackDocument::Lower(std::string(current->GetFilename()));
        if (masters.contains(key))
          continue;
        auto &list = masters[key];
        for (std::uint32_t j = 0; j < current->masterCount; ++j)
          if (current->masterPtrs && current->masterPtrs[j]) {
            list.emplace_back(current->masterPtrs[j]->GetFilename());
            files.push_back(current->masterPtrs[j]);
          }
      }
    }
  };
  // Inventory records are indexed once; searching never loads a model or scans
  // inventories.
  for (const auto type :
       {RE::FormType::Misc, RE::FormType::Ingredient, RE::FormType::AlchemyItem,
        RE::FormType::Weapon, RE::FormType::Armor, RE::FormType::Ammo,
        RE::FormType::Book, RE::FormType::SoulGem, RE::FormType::KeyMaster,
        RE::FormType::Scroll, RE::FormType::Light, RE::FormType::Apparatus})
    for (const auto *form : handler->GetFormArray(type)) {
      auto *object =
          form ? const_cast<RE::TESForm *>(form)->As<RE::TESBoundObject>()
               : nullptr;
      const auto *file = object ? object->GetFile(0) : nullptr;
      if (!object || !file || object->IsDeleted() ||
          !object->IsInventoryObject())
        continue;
      MaterialRecord record{
          object->GetFormID(), object->GetLocalFormID(),
          std::string(file->GetFilename()),
          object->GetName() ? object->GetName() : "",
          object->GetFormEditorID() ? object->GetFormEditorID() : ""};
      if (record.editorID.empty() && lookup)
        if (const auto *id = lookup(record.id))
          record.editorID = id;
      record.selectable =
          type == RE::FormType::Misc || type == RE::FormType::Ingredient;
      record.nameSearch = PackDocument::Lower(record.name);
      record.editorSearch = PackDocument::Lower(record.editorID);
      materialObjects.push_back(std::move(record));
      const auto key = PackDocument::Lower(std::string(file->GetFilename()));
      if (!masters.contains(key)) {
        auto &list = masters[key];
        for (std::uint32_t j = 0; j < file->masterCount; ++j)
          if (file->masterPtrs && file->masterPtrs[j])
            list.emplace_back(file->masterPtrs[j]->GetFilename());
      }
    }
  std::ranges::sort(materialObjects, {}, &MaterialRecord::nameSearch);
  for (std::size_t i = 0; i < materialObjects.size(); ++i)
    materialByIdentity.emplace(
        Identity(materialObjects[i].plugin, materialObjects[i].local), i);
  collect(handler->GetFormArray(RE::FormType::Static), "Static");
  collect(handler->GetFormArray(RE::FormType::MovableStatic), "Movable Static");
  collect(handler->GetFormArray(RE::FormType::Tree), "Tree");
  collect(handler->GetFormArray(RE::FormType::Furniture), "Furniture");
  collect(handler->GetFormArray(RE::FormType::Door), "Door");
  collect(handler->GetFormArray(RE::FormType::Container), "Container");
  collect(handler->GetFormArray(RE::FormType::Activator), "Activator");
  std::unordered_set<std::string> furnitureModels;
  std::unordered_map<std::string, std::string> staticAliases;
  for (const auto &entry : searchObjects) {
    const auto model = PackAssets::ModelKey(entry.model);
    if (model.empty())
      continue;
    if (entry.recordType == "Furniture" && !entry.editorID.empty())
      furnitureModels.insert(model);
    if (entry.recordType == "Static" && !entry.search.empty())
      staticAliases[model] += " " + entry.search;
  }
  for (auto &entry : searchObjects) {
    const auto model = PackAssets::ModelKey(entry.model);
    entry.hasFurnitureEquivalent = entry.recordType == "Static" &&
                                   !model.empty() &&
                                   furnitureModels.contains(model);
    // Searching the decorative record's editor ID should still find its
    // functional counterpart, while retaining that counterpart's identity.
    if (entry.recordType == "Furniture" && !model.empty())
      if (const auto found = staticAliases.find(model);
          found != staticAliases.end())
        entry.search += found->second;
  }
  editorIDsAvailable = std::ranges::any_of(
      searchObjects, [](const auto &entry) { return !entry.editorID.empty(); });
  std::set<std::string> uniquePlugins;
  for (const auto &entry : searchObjects)
    uniquePlugins.insert(entry.plugin);
  objectPlugins.assign(uniquePlugins.begin(), uniquePlugins.end());
  std::ranges::sort(searchObjects, {}, &PickerRecord::search);
  for (std::size_t i = 0; i < searchObjects.size(); ++i)
    worldPickObjects.emplace(searchObjects[i].id, i);
  for (std::size_t i = 0; i < searchObjects.size(); ++i)
    objectByIdentity.emplace(
        Identity(searchObjects[i].plugin, searchObjects[i].local), i);
  job = std::async(std::launch::async, [] {
    Result result;
    result.packs = PackPaths();
    return result;
  });
}
bool CanPickObject(RE::FormID id) { return worldPickObjects.contains(id); }
void CancelSelection() {
  if (!selectingWorldItem)
    return;
  selectingWorldItem = false;
  if (selectionWasTerrain) {
    BuildingMode::SetEnabled(false);
    Terrain::SetEnabled(true);
  } else {
    BuildingMode::SelectTool(selectionReturnTool);
  }
  Open();
}
void OpenForObject(RE::FormID id) {
  if (!selectingWorldItem || !CanPickObject(id))
    return;
  CancelSelection();
  try {
    Action({{"action", "pick"},
            {"id", id},
            {"pack", selectedPath.stem().string()}});
    View::OpenPickedItem();
  } catch (const std::exception &error) {
    message = error.what();
    stateDirty = true;
    Update();
  }
}
void RegenerateResourceIcons() {
  std::map<std::string, ThumbnailGenerator::IconRequest> unique;
  const auto add = [&](const std::string &plugin, std::uint32_t local) {
    const auto found = materialByIdentity.find(Identity(plugin, local));
    if (found == materialByIdentity.end())
      return;
    const auto path = PackDocument::IconRelative(".resources", plugin, local)
                          .generic_string();
    unique.emplace(path, ThumbnailGenerator::IconRequest{
                             materialObjects[found->second].id, path});
  };
  for (const auto &definition : PlaceableRegistry::GetDefinitions())
    for (const auto &material : definition.requiredMaterials)
      add(material.plugin, material.local);
  std::vector<ThumbnailGenerator::IconRequest> requests;
  for (const auto &[path, request] : unique)
    requests.push_back(request);
  ThumbnailGenerator::QueueRegeneration(requests);
}
void QueueResourceIcon(const std::string &plugin, std::uint32_t local,
                       bool temporary) {
  const auto found = materialByIdentity.find(Identity(plugin, local));
  if (found == materialByIdentity.end())
    return;
  const auto path =
      PackDocument::IconRelative(temporary ? ".resource-search" : ".resources",
                                 plugin, local)
          .generic_string();
  if (!requestedResourceIcons.insert(path).second)
    return;
  if (!ThumbnailGenerator::HasThumbnail(path))
    resourceIcons.emplace_back(materialObjects[found->second].id, path);
}
void Open() {
  if (!editorOpen.load())
    requestedResourceIcons.clear();
  closeSavePending = false;
  View::SetOpen(true);
  if (!View::IsOpen())
    return;
  editorOpen.store(true);
  sentAssetCatalogRevision = ~0ULL;
  Settings::SetMenuOpen(false);
  stateDirty = true;
  Update();
  RefreshPresentation();
}
void Close() {
  costBatch.clear();
  CancelShell();
  resourceIcons.clear();
  requestedResourceIcons.clear();
  if (resourceCapture)
    ThumbnailGenerator::Cancel();
  closeSavePending = dirty || job.valid() || !boundsBatch.empty();
  editorOpen.store(false);
  View::SetOpen(false);
  RefreshPresentation();
}
bool IsOpen() { return editorOpen.load(); }
void Update() {
  // Process one item per update so progress can be presented between models.
  // The operation only changes saved selection bounds, never build costs.
  if (!boundsBatch.empty() && !job.valid()) {
    auto target = std::move(boundsBatch.front());
    boundsBatch.pop_front();
    try {
      auto &item = TargetItem(target);
      const auto found = identities.find(
          Identity(item.at("plugin").get<std::string>(), Local(item)));
      auto *object =
          found == identities.end()
              ? nullptr
              : RE::TESForm::LookupByID<RE::TESBoundObject>(found->second);
      const auto model = Models::WorldModel(object);
      const std::string path =
          model && model->GetModel() ? model->GetModel() : "";
      const auto bounds = Models::GetVertexBounds(
          object, !refreshedBoundsModels.contains(path));
      if (!bounds)
        throw std::runtime_error("Model bounds unavailable");
      refreshedBoundsModels.insert(path);
      item["selectionBounds"] = {{"min", bounds->min}, {"max", bounds->max}};
      if (!target.value("draft", false))
        dirty = true;
      ++documentRevision;
    } catch (const std::exception &) {
      ++boundsFailed;
    }
    const auto completed = boundsTotal - boundsBatch.size();
    message =
        boundsBatch.empty()
            ? (boundsFailed
                   ? std::format(
                         "Bounds updated for {} / {} items. {} failed; existing bounds were kept.",
                         completed - boundsFailed, boundsTotal, boundsFailed)
                   : std::format("Bounds recalculated for {} item(s).",
                                 completed))
            : std::format("Recalculating bounds: {} / {}", completed,
                          boundsTotal);
    stateDirty = true;
  }
  if (!shellTarget && !costBatch.empty() && !job.valid()) {
    auto next = std::move(costBatch.front());
    costBatch.pop_front();
    RequestShell(std::move(next));
    stateDirty = true;
  }
  try {
    UpdateShell();
  } catch (const std::exception &error) {
    const std::string failure = error.what();
    CancelShell();
    shellStatus = failure;
    message = failure;
    stateDirty = true;
  }
  if (resourceCapture) {
    const auto progress = ThumbnailGenerator::GetProgress();
    if (progress.generation != resourceCapture->generation ||
        !progress.running) {
      const auto path =
          Data() / "SkyBuild/UI" / resourceCapture->request.second;
      if (ThumbnailGenerator::HasThumbnail(resourceCapture->request.second))
        Widgets::Invalidate(path, true);
      else if ((editorOpen.load() || BuildingMode::IsBuildMode()) &&
               (progress.generation != resourceCapture->generation ||
                progress.cancelled))
        resourceIcons.push_front(resourceCapture->request);
      resourceCapture.reset();
      stateDirty = true;
    }
  }

  if (hoverPending) {
    const auto progress = ThumbnailGenerator::GetProgress();
    if (progress.generation != hoverGeneration || !progress.running) {
      hoverPending = false;
      if (progress.generation == hoverGeneration && !progress.failed)
        Widgets::Invalidate(Data() / "SkyBuild/UI" / hoverPath);
      else {
        hoverForm = 0;
        hoverPath.clear();
      }
      stateDirty = true;
    }
  }
  ApplyJob();
  UpdatePreview();
  if (editorOpen.load() && !View::IsOpen())
    Close();
  if (closeSavePending && boundsBatch.empty() && !job.valid() &&
      !ThumbnailGenerator::GetProgress().running) {
    if (!dirty)
      closeSavePending = false;
    else
      try {
        StartPackSave();
      } catch (const std::exception &error) {
        message = error.what();
        closeSavePending = false;
        View::SetOpen(true);
        editorOpen.store(true);
        stateDirty = true;
        RefreshPresentation();
      }
  }
  if (!shellTarget &&
      (editorOpen.load() ||
       (BuildingMode::IsBuildMode() && !UI::IsGameMenuBlocking())) &&
      !resourceCapture && !resourceIcons.empty() && !job.valid() &&
      !pickerPending && !hoverPending &&
      (!pickerDesiredForm || pickerReadyForm == pickerDesiredForm ||
       pickerFailedForm == pickerDesiredForm) &&
      !ThumbnailGenerator::GetProgress().running) {
    auto request = std::move(resourceIcons.front());
    resourceIcons.pop_front();
    if (!ThumbnailGenerator::HasThumbnail(request.second)) {
      const auto before = ThumbnailGenerator::GetProgress().generation;
      ThumbnailGenerator::QueueRegeneration(
          std::vector<ThumbnailGenerator::IconRequest>{request});
      const auto after = ThumbnailGenerator::GetProgress().generation;
      if (after > before)
        resourceCapture = ResourceCapture{std::move(request), after};
      else
        resourceIcons.push_front(std::move(request));
    }
  }
  if (!editorOpen.load())
    return;
  const auto progress = ThumbnailGenerator::GetProgress();
  const auto fingerprint =
      std::format("{}:{}:{}:{}:{}:{}:{}:{}:{}:{}", documentRevision,
                  job.valid(), pickerIconRevision, pickerReadyForm,
                  progress.generation, progress.completed, progress.running,
                  progress.cancelling, progress.status, message);
  if (stateDirty || fingerprint != lastFingerprint) {
    lastFingerprint = fingerprint;
    SendState();
  }
}

} // namespace SkyBuild::UI::PackEditor
