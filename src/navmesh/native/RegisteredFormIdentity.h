#pragma once
#include <utility>

namespace SkyBuild::Navmesh {
// File history belongs to the form identity, including every contributing
// plugin. Move the complete container with that identity; SetFile(lastFile)
// would reconstruct only one entry and its null inverse retains a wrapper.
template <class Form>
void ExchangeFormIdentityFiles(Form &source, Form &work) noexcept {
  std::swap(source.sourceFiles.array, work.sourceFiles.array);
}
// Native SetFormID skips registry removal/insertion when bit 0x4000 is set.
// Factory drafts are registered before acquiring that temporary flag. Identity
// transfer must therefore update their existing registry entry as well as ID.
template <class Form, class Assign>
void AssignRegisteredFormIdentity(Form &form, unsigned id, Assign assign) {
  const auto flags = form.formFlags;
  struct RestoreFlags {
    Form &form;
    decltype(form.formFlags) savedFlags;
    ~RestoreFlags() { form.formFlags = savedFlags; }
  } restore{form, flags};
  form.formFlags &= ~0x4000u;
  assign(&form, id);
}
} // namespace SkyBuild::Navmesh
