#pragma once

// Only compile in debug mode
#ifdef _DEBUG

// =============================================================================
// THE DROP / EDGE PROBE OVERLAY (0.24.40)
// =============================================================================
// Draws the drop / edge probe's last pass in the world, over the game
// (from the D3D11 Present hook): each ray it
// cast, coloured by core/ProbeDebug.h -- red a fall, blue water, green fine,
// yellow a pick that hit something, grey out of recasts or never reached --
// with the drop (a far point: its step) or the water depth at each down
// ray's end. Drawn on ImGui's background list: under the debug windows.
//
// The rays are cast on the main thread (DropAheadProbe's PlayerCharacter::
// Update hook) and published as a picture; this projects that picture through
// the world camera at draw time (NiCamera::WorldPtToScreenPt3, as SwingSight
// does). [Debug] bDrawEnvProbe, or the Environment window's checkbox. Hidden
// while the game is paused (a menu) or loading.
// =============================================================================

namespace Huginn::UI::ProbeOverlay
{
   /// Draw the last probe pass (called each frame; nothing while the overlay is off).
   void Draw();
}

#endif  // _DEBUG
