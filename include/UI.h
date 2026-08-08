#pragma once

// SKSE Menu Framework page. Everything renders on the framework's render
// thread, so each function does the minimum work under the state lock and
// nothing at all with game objects.
//
// The vendored SKSEMenuFramework.h exposes ImGui under the ImGuiMCP namespace
// (it forwards each call into SKSEMenuFramework.dll rather than linking ImGui
// itself); UI.cpp aliases that to ImGui so the code reads like ordinary ImGui.

namespace AgencyEngine::UI
{
    // No-op when SKSEMenuFramework isn't installed.
    void Register();
}
