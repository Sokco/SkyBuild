#pragma once

#include <Windows.h>
#include <string>

namespace SkyBuild::BTPS {
// Published V0 ABI. Slot order and by-value std::string parameters must match.
// https://gitlab.com/Shrimperator/skyrim-mod-betterthirdpersonselection/-/blob/main/src/API/BTPS_API_decl.h
class API {
public:
  virtual bool GetSelectionEnabled() noexcept = 0;
  virtual bool GetWidget3DEnabled() noexcept = 0;
  virtual void HideSelectionWidget(std::string source) noexcept = 0;
  virtual void ShowSelectionWidget(std::string source) noexcept = 0;
};

inline API *GetAPI() {
  // BTPS ships with the release STL ABI. MSVC's checked Debug strings have a
  // different layout; never pass them across this interface. RelWithDebInfo
  // keeps symbols while using the compatible release ABI.
#if _ITERATOR_DEBUG_LEVEL != 0 || defined(_DEBUG)
  return nullptr;
#else
  static API *api = []() -> API * {
    const auto module = GetModuleHandleW(L"BetterThirdPersonSelection.dll");
    if (!module) return nullptr;
    enum class Version { V0 };
    using Request = void *(*)(Version);
    const auto request = reinterpret_cast<Request>(
        GetProcAddress(module, "RequestPluginAPI"));
    return request ? static_cast<API *>(request(Version::V0)) : nullptr;
  }();
  return api;
#endif
}
} // namespace SkyBuild::BTPS
