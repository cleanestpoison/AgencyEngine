#pragma once

// Papyrus-native seam used by the SkyUI MCM. The menu script owns presentation;
// this module owns validation, locking, live settings mutation, and persistence.
// No SkyUI types cross into the DLL, so SkyUI remains an optional runtime UI.

namespace AgencyEngine::MCM
{
    bool Register(RE::BSScript::IVirtualMachine* vm);
}
