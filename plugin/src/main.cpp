#include "PrismaUI_API.h"
#include <keyhandler/keyhandler.h>

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

    constexpr const char* EVT_COMMAND = "PW_PrismaCommand";  // DLL -> Papyrus (action payload)
    constexpr float       MAX_UNITS = 4096.0f;               // ~58 m search radius
    constexpr float       UNITS_PER_M = 70.0f;

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

    void ToggleMenu()
    {
        if (!g_prisma || !g_viewReady.load() || !g_prisma->IsValid(g_view)) {
            return;
        }
        if (g_prisma->HasFocus(g_view)) {
            g_prisma->Unfocus(g_view);
            g_prisma->Hide(g_view);
        } else {
            g_prisma->Show(g_view);
            g_prisma->Focus(g_view);
            PushData();
        }
    }

    // ---- JS listener callbacks (free functions: no captures) ----
    void OnJsCommand(const char* a_arg)
    {
        logger::info("JS command -> {}", a_arg ? a_arg : "(null)");
        SendCommandToPapyrus(a_arg ? a_arg : "");
    }

    void OnJsClose(const char*)
    {
        if (g_prisma && g_viewReady.load() && g_prisma->IsValid(g_view)) {
            g_prisma->Unfocus(g_view);
            g_prisma->Hide(g_view);
        }
    }

    void OnJsReady(const char*) { PushData(); }
    void OnJsRefresh(const char*) { PushData(); }

    uint32_t ReadToggleKey()
    {
        // Optional rebind via Data/SKSE/Plugins/SNPlaywright.ini -> [Controls] ToggleKey
        // (DXScanCode, decimal or 0x-hex). Default F11 (0x57).
        return static_cast<uint32_t>(
            GetPrivateProfileIntA("Controls", "ToggleKey", 0x57,
                                  "Data\\SKSE\\Plugins\\SNPlaywright.ini"));
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

        g_view = g_prisma->CreateView("Playwright/index.html", [](PrismaView a_view) {
            logger::info("Playwright view DOM ready ({})", a_view);
            g_prisma->Hide(a_view);  // start hidden; toggle reveals it
            g_viewReady.store(true);
        });

        g_prisma->RegisterJSListener(g_view, "playwright.command", OnJsCommand);
        g_prisma->RegisterJSListener(g_view, "playwright.close", OnJsClose);
        g_prisma->RegisterJSListener(g_view, "playwright.ready", OnJsReady);
        g_prisma->RegisterJSListener(g_view, "playwright.refresh", OnJsRefresh);

        KeyHandler::RegisterSink();
        KeyHandler* keys = KeyHandler::GetSingleton();
        (void)keys->Register(ReadToggleKey(), KeyEventType::KEY_DOWN,
                             []() { SKSE::GetTaskInterface()->AddTask([]() { ToggleMenu(); }); });

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
