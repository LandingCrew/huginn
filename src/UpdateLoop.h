#pragma once

/// @brief Main update callback
/// @param deltaSeconds Time since last update in seconds
/// @details Called by UpdateHandler at ~100ms intervals (configurable via Config::UPDATE_INTERVAL_MS)
void OnUpdate(float deltaSeconds);
