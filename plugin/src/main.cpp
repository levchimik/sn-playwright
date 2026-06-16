#include "PrismaUI_API.h"

#include <algorithm>
#include <string>
#include <vector>

// =============================================================================
// Playwright PrismaUI bridge (SNPlaywright.dll)
//
// The DLL owns the PrismaUI view, enumerates the nearby cast itself (read-only,
// via CommonLibSSE-NG), pushes that list to the web view, and forwards button
// clicks to Papyrus as SKSE ModEvents. ALL actions -- Say / Think / Transform /
// sleep / deaf-window -- still execute in PW_Controller.psc, unchanged.
//
//   open (hotkey)  -> [DLL] Show+Focus, enumerate cast, InteropCall pwSetData
//   JS button      -> playwright.command -> [DLL] -> ModEvent PW_PrismaCommand -> Papyrus
// =============================================================================

namespace
{
    PRISMA_UI_API::IVPrismaUI1* g_prisma = nullptr;
    PrismaView                  g_view = 0;
    std::atomic<bool>           g_viewReady{false};

    // Cached factions (looked up once, after kDataLoaded).
    RE::TESFaction* g_facAsleep = nullptr;     // PW_AsleepFaction    0x801 SNPlaywright.esp
    RE::TESFaction* g_facSleeptalk = nullptr;  // PW_SleeptalkFaction 0x803 SNPlaywright.esp
    RE::TESFaction* g_facBlacklist = nullptr;  // SkyrimNet ActorBlacklistFaction 0x12DB SkyrimNet.esp

    bool     g_pauseGame = false;   // manual "Pause" pill, persisted (ini [Behavior] PauseGame)
    bool     g_typing = false;      // transient: the panel's text box currently has focus
    bool     g_appliedPause = false;// pause value currently in effect on the live view
    uint64_t g_reapplyTick = 0;     // when we last ran an Unfocus->Focus cycle (loop guard)

    bool EffectivePause() { return g_pauseGame || g_typing; }

    constexpr const char* EVT_COMMAND = "PW_PrismaCommand";  // DLL -> Papyrus (action payload)
    constexpr float       MAX_UNITS = 4096.0f;               // ~58 m search radius
    constexpr float       UNITS_PER_M = 70.0f;
    constexpr const char* INI_PATH = "Data\\SKSE\\Plugins\\SNPlaywright.ini";

    // ---- Send the action payload to Papyrus on the main thread ----
    void SendCommandToPapyrus(std::string a_payload)
    {
        auto* task = SKSE::GetTaskInterface();
        if (!task) {
            return;
        }
        task->AddTask([a_payload]() {
            auto* src = SKSE::GetModCallbackEventSource();
            if (!src) {
                return;
            }
            SKSE::ModCallbackEvent ev{};
            ev.eventName = EVT_COMMAND;
            ev.strArg = a_payload;
            ev.numArg = 0.0f;
            ev.sender = nullptr;
            src->SendEvent(&ev);
        });
    }

    std::string JsonEsc(const char* a_in)
    {
        std::string out;
        if (!a_in) {
            return out;
        }
        for (const char* p = a_in; *p; ++p) {
            unsigned char c = static_cast<unsigned char>(*p);
            switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
            }
        }
        return out;
    }

    void LookupFactions()
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return;
        }
        g_facAsleep = dh->LookupForm<RE::TESFaction>(0x801, "SNPlaywright.esp");
        g_facSleeptalk = dh->LookupForm<RE::TESFaction>(0x803, "SNPlaywright.esp");
        g_facBlacklist = dh->LookupForm<RE::TESFaction>(0x12DB, "SkyrimNet.esp");
    }

    bool IsDirectorOn()
    {
        auto* pc = RE::PlayerCharacter::GetSingleton();
        return pc && g_facBlacklist && pc->IsInFaction(g_facBlacklist);
    }

    void PushDirector(bool a_on)
    {
        SKSE::GetTaskInterface()->AddTask([a_on]() {
            if (g_prisma && g_viewReady.load() && g_prisma->IsValid(g_view)) {
                g_prisma->InteropCall(g_view, "pwSetDirector", a_on ? "1" : "0");
            }
        });
    }

    struct NpcEntry
    {
        std::string name;
        std::string id;     // "0xXXXXXXXX"
        int         dist;   // meters
        const char* state;  // "awake" | "asleep" | "sleeptalk"
    };

    // Build the {"directorMode":bool,"npcs":[...]} payload from live game state.
    std::string BuildNpcJson()
    {
        auto* pc = RE::PlayerCharacter::GetSingleton();
        auto* lists = RE::ProcessLists::GetSingleton();
        if (!pc || !lists) {
            return "{\"directorMode\":false,\"npcs\":[]}";
        }

        const RE::NiPoint3 ppos = pc->GetPosition();
        std::vector<NpcEntry> entries;

        for (auto& handle : lists->highActorHandles) {
            auto ptr = handle.get();
            RE::Actor* a = ptr.get();
            if (!a || a == pc || a->IsDead() || a->IsDisabled() || !a->Is3DLoaded()) {
                continue;
            }
            const char* dn = a->GetDisplayFullName();
            if (!dn || !dn[0]) {
                continue;  // skip unnamed/generic actors (empty-name rows)
            }
            const float du = ppos.GetDistance(a->GetPosition());
            if (du > MAX_UNITS) {
                continue;
            }

            const char* st = "awake";
            if (g_facAsleep && a->IsInFaction(g_facAsleep)) {
                st = "asleep";
            } else if (g_facSleeptalk && a->IsInFaction(g_facSleeptalk)) {
                st = "sleeptalk";
            }

            char idbuf[16];
            std::snprintf(idbuf, sizeof(idbuf), "0x%08X", a->GetFormID());

            entries.push_back(NpcEntry{
                JsonEsc(a->GetDisplayFullName()),
                idbuf,
                static_cast<int>(du / UNITS_PER_M + 0.5f),
                st });
        }

        std::sort(entries.begin(), entries.end(),
                  [](const NpcEntry& l, const NpcEntry& r) { return l.dist < r.dist; });
        if (entries.size() > 30) {
            entries.resize(30);
        }

        const bool director = g_facBlacklist && pc->IsInFaction(g_facBlacklist);

        std::string json = "{\"directorMode\":";
        json += director ? "true" : "false";
        json += ",\"pauseGame\":";
        json += g_pauseGame ? "true" : "false";
        json += ",\"npcs\":[";
        // Player first -- selectable target for Deep Sleep / Sleep-talk (Self).
        {
            const char* pst = "awake";
            if (g_facAsleep && pc->IsInFaction(g_facAsleep)) {
                pst = "asleep";
            } else if (g_facSleeptalk && pc->IsInFaction(g_facSleeptalk)) {
                pst = "sleeptalk";
            }
            json += "{\"name\":\"";
            json += JsonEsc(pc->GetDisplayFullName());
            json += " (you)\",\"id\":\"player\",\"player\":true,\"state\":\"";
            json += pst;
            json += "\"}";
        }
        for (const auto& e : entries) {
            json += ",{\"name\":\"";
            json += e.name;
            json += "\",\"id\":\"";
            json += e.id;
            json += "\",\"dist\":";
            json += std::to_string(e.dist);
            json += ",\"state\":\"";
            json += e.state;
            json += "\"}";
        }
        json += "]}";
        return json;
    }

    // Enumerate + push -- always on the main thread.
    void PushData()
    {
        auto* task = SKSE::GetTaskInterface();
        if (!task) {
            return;
        }
        task->AddTask([]() {
            if (!g_prisma || !g_viewReady.load() || !g_prisma->IsValid(g_view)) {
                return;
            }
            const std::string json = BuildNpcJson();
            g_prisma->InteropCall(g_view, "pwSetData", json.c_str());
        });
    }

    // Re-apply the focus with the current effective pause flag. PrismaUI only
    // honours pauseGame at the moment focus is (re)acquired, so changing it live
    // requires an Unfocus -> Focus cycle (a plain re-Focus is a no-op). CEF keeps
    // the DOM's focused element across this, so the text box stays typeable.
    void ReapplyPause()
    {
        SKSE::GetTaskInterface()->AddTask([]() {
            if (!(g_prisma && g_viewReady.load() && g_prisma->IsValid(g_view) &&
                  g_prisma->HasFocus(g_view))) {
                return;
            }
            // Nothing to do if the effective pause isn't actually changing -- e.g.
            // focusing the text box while the pill already paused the game. Avoids
            // a pointless (and visible) Unfocus->Focus cycle.
            if (EffectivePause() == g_appliedPause) {
                return;
            }
            // Unfocus blurs the DOM text box and re-Focus refocuses it, each firing
            // a JS focus/blur. Stamp the time so OnJsConfig can ignore those
            // self-inflicted echoes (else pausing thrashes).
            g_reapplyTick = GetTickCount64();
            g_appliedPause = EffectivePause();
            g_prisma->Unfocus(g_view);
            g_prisma->Focus(g_view, g_appliedPause);
        });
    }

    void ToggleMenu()
    {
        if (!g_prisma || !g_viewReady.load() || !g_prisma->IsValid(g_view)) {
            return;
        }
        if (g_prisma->HasFocus(g_view)) {
            g_typing = false;  // closing clears the transient typing-pause
            g_appliedPause = false;
            g_prisma->Unfocus(g_view);
            g_prisma->InteropCall(g_view, "pwSetOpen", "0");  // collapse to dot
        } else {
            g_appliedPause = EffectivePause();
            g_prisma->Focus(g_view, g_appliedPause);
            g_prisma->InteropCall(g_view, "pwSetOpen", "1");  // expand panel
            PushData();
        }
    }

    // ---- ModEvent sink: Papyrus -> DLL (the MCM-bound key fires PW_PrismaToggle) ----
    class PrismaModEventSink : public RE::BSTEventSink<SKSE::ModCallbackEvent>
    {
    public:
        static PrismaModEventSink* GetSingleton()
        {
            static PrismaModEventSink instance;
            return &instance;
        }
        RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* a_event,
                                              RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
        {
            if (a_event) {
                const std::string nm = a_event->eventName.c_str();
                if (nm == "PW_PrismaToggle") {
                    SKSE::GetTaskInterface()->AddTask([]() { ToggleMenu(); });
                } else if (nm == "PW_PrismaDirector") {
                    PushDirector(a_event->numArg >= 0.5f);  // recording dot on/off
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        PrismaModEventSink() = default;
    };

    void CollapseFromCode()  // close the panel from C++ (main thread)
    {
        SKSE::GetTaskInterface()->AddTask([]() {
            if (g_prisma && g_viewReady.load() && g_prisma->IsValid(g_view)) {
                g_typing = false;
                g_appliedPause = false;
                g_prisma->Unfocus(g_view);
                g_prisma->InteropCall(g_view, "pwSetOpen", "0");
            }
        });
    }

    // ---- Escape handling ----
    // PrismaUI views don't get Escape on their own (the game claims it), so we
    // watch the raw input stream while the panel is focused. Observe-only: if
    // typing, the first Escape just exits the text box (via the view); otherwise
    // it closes the panel.
    class EscInputSink : public RE::BSTEventSink<RE::InputEvent*>
    {
    public:
        static EscInputSink* GetSingleton()
        {
            static EscInputSink instance;
            return &instance;
        }
        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_events,
                                              RE::BSTEventSource<RE::InputEvent*>*) override
        {
            if (!a_events || !g_prisma || !g_viewReady.load() || !g_prisma->IsValid(g_view) ||
                !g_prisma->HasFocus(g_view)) {
                return RE::BSEventNotifyControl::kContinue;
            }
            for (auto* e = *a_events; e; e = e->next) {
                if (e->eventType != RE::INPUT_EVENT_TYPE::kButton) {
                    continue;
                }
                auto* be = e->AsButtonEvent();
                if (!be || be->device.get() != RE::INPUT_DEVICE::kKeyboard) {
                    continue;
                }
                if (be->GetIDCode() == 0x01 /* DIK_ESCAPE */ && be->IsDown()) {
                    if (g_typing) {
                        // First Escape: exit the text box (the view blurs + unpauses).
                        SKSE::GetTaskInterface()->AddTask([]() {
                            if (g_prisma && g_viewReady.load() && g_prisma->IsValid(g_view)) {
                                g_prisma->InteropCall(g_view, "pwBlurText", "");
                            }
                        });
                    } else {
                        CollapseFromCode();
                    }
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        EscInputSink() = default;
    };

    // ---- JS listener callbacks (free functions: no captures) ----
    void OnJsCommand(const char* a_arg)
    {
        logger::info("JS command -> {}", a_arg ? a_arg : "(null)");
        SendCommandToPapyrus(a_arg ? a_arg : "");
    }

    void OnJsClose(const char*)
    {
        // Runs on the CEF/JS thread. Marshal to the main thread before touching the
        // view -- calling Unfocus off-thread glitches the player's run/walk state
        // (that's why the X button flipped walk mode but the combo, which already
        // routes through the main thread, did not).
        SKSE::GetTaskInterface()->AddTask([]() {
            if (g_prisma && g_viewReady.load() && g_prisma->IsValid(g_view)) {
                g_typing = false;
                g_appliedPause = false;
                g_prisma->Unfocus(g_view);
                g_prisma->InteropCall(g_view, "pwSetOpen", "0");  // collapse to dot
            }
        });
    }

    void OnJsRefresh(const char*) { PushData(); }

    // Live settings from the panel. arg = "key|value", e.g. "pause|1".
    // Pure C++ state (no Papyrus involved); applied live + persisted to the ini.
    void OnJsConfig(const char* a_arg)
    {
        if (!a_arg) {
            return;
        }
        std::string s = a_arg;
        auto bar = s.find('|');
        if (bar == std::string::npos) {
            return;
        }
        const std::string key = s.substr(0, bar);
        const std::string val = s.substr(bar + 1);
        const bool on = (val == "1" || val == "true");
        if (key == "pause") {
            // Manual pill -- persisted.
            g_pauseGame = on;
            WritePrivateProfileStringA("Behavior", "PauseGame", g_pauseGame ? "1" : "0", INI_PATH);
            logger::info("Config pause(pill) -> {}", g_pauseGame);
            ReapplyPause();
        } else if (key == "typing") {
            // Text box gained/lost focus -- transient auto-pause so keystrokes
            // don't leak to other mods' hotkeys while composing a line.
            // Ignore the focus/blur echo from our own Unfocus->Focus cycle, or the
            // pause state would thrash (blur->unpause->focus->pause->...).
            if (GetTickCount64() - g_reapplyTick < 500) {
                return;
            }
            if (g_typing == on) {
                return;
            }
            g_typing = on;
            ReapplyPause();
        }
    }

    bool ReadPauseGame()
    {
        // [Behavior] PauseGame: 0 = world keeps running while the panel is open
        // (default — you direct the scene live), 1 = freeze the game.
        return GetPrivateProfileIntA("Behavior", "PauseGame", 0, INI_PATH) != 0;
    }

    void SetupLog()
    {
        auto path = SKSE::log::log_directory();
        if (!path) {
            return;
        }
        *path /= "SNPlaywright.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
        auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
    }

    void OnDataLoaded()
    {
        g_prisma = static_cast<PRISMA_UI_API::IVPrismaUI1*>(
            PRISMA_UI_API::RequestPluginAPI(PRISMA_UI_API::InterfaceVersion::V1));
        if (!g_prisma) {
            logger::error("PrismaUI API unavailable -- is Prisma UI installed/loaded?");
            return;
        }

        LookupFactions();
        g_pauseGame = ReadPauseGame();

        g_view = g_prisma->CreateView("Playwright/index.html", [](PrismaView a_view) {
            logger::info("Playwright view DOM ready ({})", a_view);
            // Register listeners HERE (DOM is ready / globals exist) with FLAT
            // names -- PrismaUI creates window.<name> unconditionally. Doing this
            // before DOM-ready, or with dotted names, silently fails to bind.
            g_prisma->RegisterJSListener(a_view, "pwCommand", OnJsCommand);
            g_prisma->RegisterJSListener(a_view, "pwClose", OnJsClose);
            g_prisma->RegisterJSListener(a_view, "pwRefresh", OnJsRefresh);
            g_prisma->RegisterJSListener(a_view, "pwConfig", OnJsConfig);
            // The view stays SHOWN at all times so its corner dot widget is always
            // visible; "closed" just collapses to the dot (unfocused, click-through
            // via pointer-events:none on empty areas). It is never Hidden.
            g_viewReady.store(true);
            g_prisma->InteropCall(a_view, "pwSetOpen", "0");
            g_prisma->InteropCall(a_view, "pwSetDirector", IsDirectorOn() ? "1" : "0");
            logger::info("Playwright JS listeners registered.");
        });

        // The panel is opened solely by the MCM-bound key: PW_Controller fires
        // PW_PrismaToggle, we catch it here.
        if (auto* src = SKSE::GetModCallbackEventSource()) {
            src->AddEventSink(PrismaModEventSink::GetSingleton());
        }

        // Watch raw input for Escape while the panel is focused (the game claims
        // Escape, so the view can't see it on its own).
        if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
            idm->AddEventSink(EscInputSink::GetSingleton());
        }

        logger::info("Playwright PrismaUI bridge initialized.");
    }

    void SKSEMessageHandler(SKSE::MessagingInterface::Message* a_message)
    {
        if (a_message->type == SKSE::MessagingInterface::kDataLoaded) {
            OnDataLoaded();
        }
    }
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    REL::Module::reset();
    SetupLog();

    auto* messaging =
        reinterpret_cast<SKSE::MessagingInterface*>(a_skse->QueryInterface(SKSE::LoadInterface::kMessaging));
    if (!messaging) {
        logger::critical("Failed to get messaging interface; plugin will not load.");
        return false;
    }

    SKSE::Init(a_skse);
    SKSE::AllocTrampoline(1 << 10);

    messaging->RegisterListener("SKSE", SKSEMessageHandler);
    logger::info("SNPlaywright loaded.");
    return true;
}
