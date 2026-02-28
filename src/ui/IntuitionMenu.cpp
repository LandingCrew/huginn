#include "IntuitionMenu.h"
#include "IntuitionSettings.h"
#include "slot/SlotAssignment.h"
#include "candidate/CandidateTypes.h"
#include "state/PlayerActorState.h"
#include "weapon/WeaponData.h"

#include <algorithm>
#include <format>

namespace Huginn::UI
{
    // ─── Lifecycle ───────────────────────────────────────────

    void IntuitionMenu::Register()
    {
        auto* ui = RE::UI::GetSingleton();
        if (ui) {
            ui->Register(MENU_NAME, CreateInstance);
            logger::info("IntuitionMenu registered"sv);
        }
    }

    RE::IMenu* IntuitionMenu::CreateInstance()
    {
        return new IntuitionMenu();
    }

    void IntuitionMenu::Show()
    {
        if (!IntuitionSettings::GetSingleton().IsEnabled()) {
            logger::info("IntuitionMenu::Show() - disabled via INI, skipping"sv);
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            logger::warn("IntuitionMenu::Show() - RE::UI is null"sv);
            return;
        }

        bool alreadyOpen = ui->IsMenuOpen(MENU_NAME);
        auto* singleton = GetSingleton();
        logger::info("IntuitionMenu::Show() - alreadyOpen={}, singleton={}"sv,
            alreadyOpen, singleton != nullptr);

        if (!alreadyOpen) {
            auto* msgQueue = RE::UIMessageQueue::GetSingleton();
            if (msgQueue) {
                msgQueue->AddMessage(MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
                logger::info("IntuitionMenu::Show() - kShow message queued"sv);
            } else {
                logger::warn("IntuitionMenu::Show() - UIMessageQueue is null"sv);
            }
        }
    }

    void IntuitionMenu::Hide()
    {
        auto* ui = RE::UI::GetSingleton();
        if (ui && ui->IsMenuOpen(MENU_NAME)) {
            auto* msgQueue = RE::UIMessageQueue::GetSingleton();
            if (msgQueue) {
                msgQueue->AddMessage(MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
                logger::info("IntuitionMenu::Hide() - kHide message queued"sv);
            }
        } else {
            logger::debug("IntuitionMenu::Hide() - not open or UI null"sv);
        }
    }

    IntuitionMenu* IntuitionMenu::GetSingleton()
    {
        // Stage 3a: Thread-safe singleton access
        return s_instance.load(std::memory_order_acquire);
    }

    // ─── Constructor ─────────────────────────────────────────

    IntuitionMenu::IntuitionMenu()
    {
        logger::info("IntuitionMenu::IntuitionMenu() - constructor called (previous singleton={})"sv,
            s_instance.load(std::memory_order_acquire) != nullptr);

        // HUD-style flags: always visible, updated each frame, doesn't block saving
        auto& flags = menuFlags;
        flags.set(RE::UI_MENU_FLAGS::kAlwaysOpen);
        flags.set(RE::UI_MENU_FLAGS::kRequiresUpdate);
        flags.set(RE::UI_MENU_FLAGS::kAllowSaving);

        depthPriority = 0;

        // Stage 3a: Thread-safe singleton initialization
        // Set singleton BEFORE any deferred calls (SetPosition, SetWidgetAlpha)
        // so that AddUITask lambdas can find us via GetSingleton().
        s_instance.store(this, std::memory_order_release);

        // Load the SWF — BSScaleformManager prepends "Data/Interface/" and appends ".swf"
        auto* scaleformManager = RE::BSScaleformManager::GetSingleton();
        if (scaleformManager && scaleformManager->LoadMovie(this, uiMovie, FILE_NAME.data())) {
            logger::info("IntuitionMenu SWF loaded"sv);

            // No mouse cursor — this is a passive HUD element
            uiMovie->SetMouseCursorCount(0);

            // Grab the widget instance placed at _root.widget in the SWF
            uiMovie->GetVariable(&m_widget, "_root.widget");

            if (m_widget.IsObject()) {
                logger::info("IntuitionMenu: _root.widget found (type={})"sv,
                    static_cast<int>(m_widget.GetType()));
            } else {
                logger::error("IntuitionMenu: _root.widget not found (type={})"sv,
                    static_cast<int>(m_widget.GetType()));
            }

            // Log viewport info
            RE::GViewport viewport;
            uiMovie->GetViewport(&viewport);
            logger::info("IntuitionMenu viewport: {}x{} at ({},{}) buf={}x{}"sv,
                viewport.width, viewport.height, viewport.left, viewport.top,
                viewport.bufferWidth, viewport.bufferHeight);

            auto scaleMode = uiMovie->GetViewScaleMode();
            logger::info("IntuitionMenu scaleMode={}"sv, static_cast<int>(scaleMode));

            // Apply position and scale from INI settings
            const auto& settings = IntuitionSettings::GetSingleton();
            SetPosition(settings.GetPositionX(), settings.GetPositionY());
            SetWidgetAlpha(settings.GetAlpha());

            // Apply user-configurable scale via AS2 _xscale/_yscale (percentage-based)
            float scalePct = settings.GetScale();
            RE::GFxValue root;
            uiMovie->GetVariable(&root, "_root");
            if (root.IsObject()) {
                root.SetMember("_xscale", static_cast<double>(scalePct));
                root.SetMember("_yscale", static_cast<double>(scalePct));
                logger::info("IntuitionMenu: scale set to {}%"sv, scalePct);
            }

            // Push INI toggles to AS2 widget
            if (m_widget.IsObject()) {
                RE::GFxValue caArg{ static_cast<double>(settings.GetChildAlpha()) };
                m_widget.Invoke("setChildAlpha", nullptr, &caArg, 1);

                RE::GFxValue reArg{ static_cast<double>(static_cast<int>(settings.GetRefreshEffect())) };
                m_widget.Invoke("setRefreshEffect", nullptr, &reArg, 1);

                RE::GFxValue seArg{ static_cast<double>(static_cast<int>(settings.GetSlotEffect())) };
                m_widget.Invoke("setSlotEffect", nullptr, &seArg, 1);

                RE::GFxValue rsArg{ static_cast<double>(settings.GetRefreshStrength()) };
                m_widget.Invoke("setRefreshStrength", nullptr, &rsArg, 1);
            }
        } else {
            logger::error("IntuitionMenu: failed to load {}.swf"sv, FILE_NAME);
        }
    }

    // ─── IMenu Overrides ─────────────────────────────────────

    void IntuitionMenu::AdvanceMovie(float a_interval, [[maybe_unused]] std::uint32_t a_currentTime)
    {
        if (uiMovie) {
            uiMovie->Advance(a_interval);
        }
        // Drive animation tick from C++ — Scaleform GFx in Skyrim does not
        // reliably fire AS2 onEnterFrame, so we call tick() directly every
        // render frame with the actual delta time from the game engine.
        if (m_widget.IsObject()) {
            RE::GFxValue arg{ static_cast<double>(a_interval) };
            m_widget.Invoke("tick", nullptr, &arg, 1);
        }
    }

    // ─── Public API ──────────────────────────────────────────
    // THREAD SAFETY: All public API methods defer GFx work to the UI thread
    // via SKSE::GetTaskInterface()->AddUITask().  The update handler calls
    // these from its own thread; Scaleform's GFx runtime is NOT thread-safe,
    // so direct cross-thread Invoke / CreateString / SetMember would race
    // with the render thread.  String parameters are copied into the lambda
    // capture so the backing storage outlives the caller's scope.

    void IntuitionMenu::SetSlot(int index, std::string_view name,
        IntuitionSlotType type, double confidence, std::string_view detail,
        Slot::SlotVisualState visualState)
    {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) return;

        tasks->AddUITask(
            [index, name = std::string(name), type, confidence,
             detail = std::string(detail), visualState]() {
                auto* self = GetSingleton();
                if (!self || !self->m_widget.IsObject() || !self->uiMovie) return;

                // CreateString copies into the movie's managed heap so the AS2 VM
                // can safely reference the data across frames.
                std::array<RE::GFxValue, 6> args;  // One more argument
                args[0] = static_cast<double>(index);
                self->uiMovie->CreateString(&args[1], name.c_str());
                args[2] = static_cast<double>(static_cast<int>(type));
                args[3] = confidence;
                self->uiMovie->CreateString(&args[4], detail.c_str());
                args[5] = static_cast<double>(static_cast<int>(visualState));  // NEW

                self->m_widget.Invoke("setSlot", nullptr, args.data(), args.size());
            });
    }

    void IntuitionMenu::ClearSlot(int index)
    {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) return;

        tasks->AddUITask([index]() {
            auto* self = GetSingleton();
            if (!self || !self->m_widget.IsObject()) return;

            RE::GFxValue arg{ static_cast<double>(index) };
            self->m_widget.Invoke("clearSlot", nullptr, &arg, 1);
        });
    }

    void IntuitionMenu::SetSlotCount(int count)
    {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) return;

        tasks->AddUITask([count]() {
            auto* self = GetSingleton();
            if (!self || !self->m_widget.IsObject()) return;

            RE::GFxValue arg{ static_cast<double>(count) };
            self->m_widget.Invoke("setSlotCount", nullptr, &arg, 1);
        });
    }

    void IntuitionMenu::SetPage(int current, int total, std::string_view name)
    {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) return;

        tasks->AddUITask([current, total, name = std::string(name)]() {
            auto* self = GetSingleton();
            if (!self || !self->m_widget.IsObject() || !self->uiMovie) return;

            std::array<RE::GFxValue, 3> args;
            args[0] = static_cast<double>(current);
            args[1] = static_cast<double>(total);
            self->uiMovie->CreateString(&args[2], name.c_str());

            self->m_widget.Invoke("setPage", nullptr, args.data(), args.size());
        });
    }

    void IntuitionMenu::SetUrgent(int index, bool active)
    {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) return;

        tasks->AddUITask([index, active]() {
            auto* self = GetSingleton();
            if (!self || !self->m_widget.IsObject()) return;

            std::array<RE::GFxValue, 2> args;
            args[0] = static_cast<double>(index);
            args[1] = active;

            self->m_widget.Invoke("setUrgent", nullptr, args.data(), args.size());
        });
    }

    void IntuitionMenu::SetWidgetAlpha(double alpha)
    {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) return;

        tasks->AddUITask([alpha]() {
            auto* self = GetSingleton();
            if (!self || !self->m_widget.IsObject()) return;

            RE::GFxValue arg{ alpha };
            self->m_widget.Invoke("setWidgetAlpha", nullptr, &arg, 1);
        });
    }

    void IntuitionMenu::SetPosition(float percentX, float percentY)
    {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) return;

        tasks->AddUITask([percentX, percentY]() {
            auto* self = GetSingleton();
            if (!self || !self->uiMovie) return;

            RE::GViewport viewport;
            self->uiMovie->GetViewport(&viewport);

            const float scaleX = static_cast<float>(viewport.width)  / IntuitionDefaults::STAGE_WIDTH;
            const float scaleY = static_cast<float>(viewport.height) / IntuitionDefaults::STAGE_HEIGHT;
            const float scale  = std::min(scaleX, scaleY);

            const float renderedW = IntuitionDefaults::STAGE_WIDTH  * scale;
            const float renderedH = IntuitionDefaults::STAGE_HEIGHT * scale;
            const float offsetX   = (static_cast<float>(viewport.width)  - renderedW) / 2.0f;
            const float offsetY   = (static_cast<float>(viewport.height) - renderedH) / 2.0f;

            const float screenX = static_cast<float>(viewport.width)  * percentX / 100.0f;
            const float screenY = static_cast<float>(viewport.height) * percentY / 100.0f;
            const float stageX  = (screenX - offsetX) / scale;
            const float stageY  = (screenY - offsetY) / scale;

            RE::GFxValue root;
            self->uiMovie->GetVariable(&root, "_root");
            if (root.IsObject()) {
                root.SetMember("_x", static_cast<double>(stageX));
                root.SetMember("_y", static_cast<double>(stageY));
                logger::info("IntuitionMenu: position {}%,{}% -> stage ({:.1f}, {:.1f}) [viewport {}x{}, scale {:.2f}, offset ({:.0f},{:.0f})]"sv,
                    percentX, percentY, stageX, stageY,
                    viewport.width, viewport.height, scale, offsetX, offsetY);
            }
        });
    }

    // ─── Visibility ─────────────────────────────────────────

    void IntuitionMenu::SetVisible(bool a_visible)
    {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) return;

        // Only log when visibility actually changes
        static bool s_lastVisible = false;
        if (a_visible != s_lastVisible) {
            logger::debug("IntuitionMenu::SetVisible({}) queued"sv, a_visible);
            s_lastVisible = a_visible;
        }

        tasks->AddUITask([a_visible]() {
            auto* self = GetSingleton();
            if (!self || !self->uiMovie) {
                logger::warn("IntuitionMenu::SetVisible({}) - singleton or uiMovie null"sv, a_visible);
                return;
            }

            RE::GFxValue root;
            self->uiMovie->GetVariable(&root, "_root");
            if (root.IsObject()) {
                RE::GFxValue::DisplayInfo di;
                root.GetDisplayInfo(&di);
                di.SetVisible(a_visible);
                root.SetDisplayInfo(di);
            }
        });
    }

    // ─── Hot Reload ─────────────────────────────────────────

    void IntuitionMenu::ReapplySettings()
    {
        // Re-read INI on calling thread (file I/O is thread-safe), then delegate
        auto& settings = IntuitionSettings::GetSingleton();
        settings.LoadFromFile("Data/SKSE/Plugins/Huginn.ini");
        ReapplySettings(settings.BuildConfig());
    }

    void IntuitionMenu::ReapplySettings(const IntuitionConfig& config)
    {
        // Position + alpha go through the public API (already deferred via AddUITask)
        SetPosition(config.positionX, config.positionY);
        SetWidgetAlpha(config.alpha);

        // Capture remaining settings for a single deferred GFx batch
        float scalePct = config.scale;
        double childAlpha = config.childAlpha;
        int refreshEffect = static_cast<int>(config.refreshEffect);
        int slotEffect = static_cast<int>(config.slotEffect);
        double refreshStrength = config.refreshStrength;

        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) return;

        tasks->AddUITask(
            [scalePct, childAlpha, refreshEffect, slotEffect, refreshStrength]() {
                auto* self = GetSingleton();
                if (!self || !self->uiMovie || !self->m_widget.IsObject()) {
                    logger::warn("IntuitionMenu::ReapplySettings(config): widget not ready"sv);
                    return;
                }

                // Scale via _root._xscale/_yscale
                RE::GFxValue root;
                self->uiMovie->GetVariable(&root, "_root");
                if (root.IsObject()) {
                    root.SetMember("_xscale", static_cast<double>(scalePct));
                    root.SetMember("_yscale", static_cast<double>(scalePct));
                }

                // Effect modes + strength
                RE::GFxValue caArg{ childAlpha };
                self->m_widget.Invoke("setChildAlpha", nullptr, &caArg, 1);

                RE::GFxValue reArg{ static_cast<double>(refreshEffect) };
                self->m_widget.Invoke("setRefreshEffect", nullptr, &reArg, 1);

                RE::GFxValue seArg{ static_cast<double>(slotEffect) };
                self->m_widget.Invoke("setSlotEffect", nullptr, &seArg, 1);

                RE::GFxValue rsArg{ refreshStrength };
                self->m_widget.Invoke("setRefreshStrength", nullptr, &rsArg, 1);

                logger::info("IntuitionMenu: settings reapplied from config snapshot"sv);
            });
    }

    // ─── Type Mapping ─────────────────────────────────────────

    IntuitionSlotType IntuitionMenu::MapSlotContentType(SlotContentType type)
    {
        switch (type) {
            case SlotContentType::Empty:         return IntuitionSlotType::kEmpty;
            case SlotContentType::NoMatch:       return IntuitionSlotType::kNoMatch;
            case SlotContentType::Spell:         return IntuitionSlotType::kSpell;
            case SlotContentType::Wildcard:      return IntuitionSlotType::kWildcard;
            case SlotContentType::Potion:        return IntuitionSlotType::kHealthPotion;  // Generic potion fallback
            case SlotContentType::HealthPotion:  return IntuitionSlotType::kHealthPotion;
            case SlotContentType::MagickaPotion: return IntuitionSlotType::kMagickaPotion;
            case SlotContentType::StaminaPotion: return IntuitionSlotType::kStaminaPotion;
            case SlotContentType::MeleeWeapon:   return IntuitionSlotType::kMeleeWeapon;
            case SlotContentType::RangedWeapon:  return IntuitionSlotType::kRangedWeapon;
            case SlotContentType::SoulGem:       return IntuitionSlotType::kSpell;  // Use spell visual for soul gems
            default:                             return IntuitionSlotType::kEmpty;
        }
    }

    // ─── Detail String Builder ────────────────────────────────

    std::string IntuitionMenu::BuildSlotDetail(
        const Slot::SlotAssignment& assignment,
        DisplayMode mode,
        const State::PlayerActorState& playerState)
    {
        if (mode == DisplayMode::Minimal || !assignment.HasCandidate())
            return "";

        const auto& scored = *assignment.candidate;
        std::string detail;

        // === Normal mode: type-specific details ===
        if (auto* spell = scored.TryAs<Candidate::SpellCandidate>()) {
            if (spell->baseCost > 0) {
                detail = std::format("{} MP{}",
                    spell->baseCost,
                    spell->isConcentration ? "/s" : "");
            }
        }
        else if (auto* weapon = scored.TryAs<Candidate::WeaponCandidate>()) {
            if (weapon->type == Weapon::WeaponType::Bow ||
                weapon->type == Weapon::WeaponType::Crossbow) {
                // Ranged: total damage (bow + arrow) + ammo count
                int totalDmg = static_cast<int>(weapon->baseDamage + playerState.equippedAmmoDamage);
                detail = std::format("{} dmg", totalDmg);
                int32_t count = (weapon->type == Weapon::WeaponType::Bow)
                    ? playerState.arrowCount : playerState.boltCount;
                if (count > 0) {
                    if (!playerState.equippedAmmoName.empty()) {
                        detail += std::format(" \xC2\xB7 {} x {}", count, playerState.equippedAmmoName);
                    } else {
                        detail += std::format(" \xC2\xB7 {} {}",
                            count,
                            (weapon->type == Weapon::WeaponType::Bow) ? "arrows" : "bolts");
                    }
                }
            }
            else if (weapon->type == Weapon::WeaponType::Staff) {
                // Staff: charge % only
                if (weapon->hasEnchantment) {
                    detail = std::format("{}%",
                        static_cast<int>(weapon->GetChargePercent() * 100));
                }
            }
            else {
                // Melee: damage + optional charge
                detail = std::format("{} dmg", static_cast<int>(weapon->baseDamage));
                if (weapon->hasEnchantment) {
                    detail += std::format(" \xC2\xB7 {}%",
                        static_cast<int>(weapon->GetChargePercent() * 100));
                }
            }
        }
        else if (auto* item = scored.TryAs<Candidate::ItemCandidate>()) {
            if (item->count > 0) detail = std::format("x{}", item->count);
        }
        else if (auto* scroll = scored.TryAs<Candidate::ScrollCandidate>()) {
            if (scroll->count > 0) detail = std::format("x{}", scroll->count);
        }
        else if (auto* ammo = scored.TryAs<Candidate::AmmoCandidate>()) {
            if (ammo->baseDamage > 0) {
                detail = std::format("{} dmg", static_cast<int>(ammo->baseDamage));
                if (ammo->count > 0)
                    detail += std::format(" \xC2\xB7 x{}", ammo->count);
            } else if (ammo->count > 0) {
                detail = std::format("x{}", ammo->count);
            }
        }

        // === Verbose mode: append score breakdown ===
        if (mode == DisplayMode::Verbose) {
            const auto& b = scored.breakdown;
            std::string scores = std::format("ctx:{:.1f} q:{:.1f} p:{:.1f}",
                b.contextWeight, b.qValue, b.prior);
            if (b.recencyBoost > 0.0f) {
                scores += std::format(" rec:{:.1f}", b.recencyBoost);
            }
            if (!detail.empty()) {
                detail += std::format(" \xC2\xB7 {}", scores);
            } else {
                detail = scores;
            }
        }

        return detail;
    }
}
