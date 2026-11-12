#pragma once
namespace SkyBuild::ControlBindings { struct Event; }

namespace SkyBuild::Terrain {
constexpr std::uint32_t SaveRecord = 0x4E525254; // TRRN
enum class Tool { Sculpt, Level, Smooth, Grass, Idle };
Tool GetTool();
float BrushRadius();
bool CanUndo();
bool CanRedo();
int BuildProgress(); // -1 when idle, otherwise completed chunks (0..16).
void SelectWheelAction(int sector);
void Initialize();
void SetStrengthPerSecond(float strength);
void SetSceneLoaded(bool loaded);
void Update(RE::PlayerCharacter *player);
bool HandleButton(const ControlBindings::Event *button);
bool Ready();
bool Enabled();
void SetEnabled(bool enabled);
// End held input when an interactive workspace takes control, without undoing
// edits.
void SuspendInput();
void Reset(bool preserveGrass = false);
void Save(SKSE::SerializationInterface *serialization);
void Load(SKSE::SerializationInterface *serialization, std::uint32_t version,
          std::uint32_t length);
} // namespace SkyBuild::Terrain
