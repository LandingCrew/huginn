#pragma once

// =============================================================================
// TEST RUNNERS - Integration and unit tests (debug mode only)
// =============================================================================

void RunMultiplicativeScoringTests();  // Stage 2d: Multiplicative formula tests
void RunRegressionTests();             // Regression suite for v1.0 refactor validation
void RunSpellRegistryTests();
void RunItemClassifierTests();
void RunItemRegistryTests();
void RunWeaponRegistryTests();
void RunCosaveTests();
void RunStateFeaturesTests();
void RunFeatureQLearnerTests();
void RunUnitTests();

// =============================================================================
// CONSOLE COMMANDS - Manual testing via Skyrim console (debug mode only)
// =============================================================================

#ifndef NDEBUG
void ConsoleCmd_ShowEquippedSpells();

#ifdef _DEBUG
void ConsoleCmd_ToggleStateManagerDebug();
void ConsoleCmd_ForceUpdateStateManager();
#endif

#endif  // !NDEBUG
