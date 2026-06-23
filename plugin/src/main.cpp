#include "PrismaUI_API.h"

#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
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

    // Factions referenced by "faction:<formid>~<plugin>" tokens in the installed button
    // fragments, auto-discovered + resolved once at startup. Per-actor IsInFaction results
    // are pushed to the view (npc.fx) so a button's showWhen can gate on them. Read-only
    // after startup, so BuildNpcJson reads it on the main thread without locking.
    std::vector<std::pair<std::string, RE::TESFaction*>> g_menuFactions;
    RE::BGSPerk*    g_perkTelepathy = nullptr;       // SkyrimNet_TelepathyPerk          0x12DD SkyrimNet.esp
    RE::BGSPerk*    g_perkTelepathyCanon = nullptr;  // SkyrimNet_TelepathyCanonicalPerk 0x12DE SkyrimNet.esp

    bool              g_pauseGame = false;   // manual "Pause" pill, persisted (ini [Behavior] PauseGame)
    bool              g_captureKeys = false; // [Behavior] CaptureKeys: steal keys from other mods while open
    std::atomic<bool> g_allActors{false};    // [Behavior] AllActors: union the local engine enumeration into the
                                             // cast so SkyrimNet-ineligible/unprofiled actors (generics, voiceless)
                                             // show up too -- read on the main thread, flipped from the CEF thread
    int               g_maxCast = 50;        // [Behavior] MaxCast: max rows in the cast list (nearest first)
    std::atomic<bool> g_typing{false};       // transient: the panel's text box currently has focus
    std::atomic<bool> g_panelOpen{false};    // panel expanded + focused (vs collapsed to the corner dot)
    std::atomic<bool> g_inGame{false};       // a save is loaded / player is in the world (false at the main menu).
                                             // Gates the persistent conversation log so it doesn't float on the title screen.
    std::atomic<bool> g_lookMode{false};     // RMB held: panel temporarily unfocused so you can look + move
    RE::Setting*      g_alwaysRun = nullptr; // "bAlwaysRunByDefault:Controls" (cached) -- read for run/walk resync
    bool              g_appliedPause = false; // pause value currently in effect on the live view
    uint64_t          g_reapplyTick = 0;      // when we last ran an Unfocus->Focus cycle (loop guard)
    std::atomic<int>  g_pendingTypingEchoes{0}; // exact # of self-inflicted typing focus/blur echoes
                                                // still expected from the current Unfocus->Focus cycle

    bool EffectivePause() { return g_pauseGame || g_typing; }

    // SkyrimNet local REST API (read from its WebServer.yaml; respects the user's port).
    std::string g_apiHost = "127.0.0.1";
    int         g_apiPort = 8080;

    // Live interaction radius mirrored from SkyrimNet (GET /config?api=get&name=game).
    // Whisper mode is a global SkyrimNet toggle that swaps interaction.maxDistance between
    // normalMaxDistance and whisperMaxDistance; we read the active value and gate our cast
    // list on it so the panel shows exactly who SkyrimNet considers in-scene. Defaults match
    // SkyrimNet's stock normal radius (used until the first successful /config fetch, or if
    // the web server is off).
    std::atomic<float> g_interactRadius{1600.0f};  // active interaction.maxDistance
    std::atomic<float> g_normalDist{1600.0f};      // interaction.normalMaxDistance
    std::atomic<float> g_whisperDist{200.0f};      // interaction.whisperMaxDistance
    std::atomic<bool>  g_whisperOn{false};         // active radius ~= whisper radius
    std::atomic<bool>  g_continuousOn{false};      // gamemaster-status continuous_scene_mode (live)
    // get_nearby_npc_list's awareness distance = interaction.maxDistance * this; we pass it to
    // nearby-actors so our cast == SkyrimNet's "available to speak with" set.
    std::atomic<float> g_awarenessMult{2.0f};      // interaction.nearbyNPCAwarenessMultiplier

    // GameMaster + world-event-reaction state, mirrored from SkyrimNet config so the panel
    // pills show the real on/off (and stay correct even when toggled via SkyrimNet's own
    // hotkeys). GameMaster: live gamemaster-status agent_enabled (the runtime agent toggle,
    // NOT static config gamemaster.enabled). NPC reactions: Events-config
    // global.npcReactionsEnabled. Defaults match SkyrimNet's stock (both on).
    std::atomic<bool> g_gamemasterOn{true};    // gamemaster-status agent_enabled
    std::atomic<bool> g_npcReactionsOn{true};  // Events global.npcReactionsEnabled

    // Snapshot of SkyrimNet's nearby/addressable cast (from /game-data?api=nearby-actors), refreshed
    // off-thread. This is the set SkyrimNet treats as in-scene (awareness radius + ActorFilter; NO
    // line-of-sight filtering -- occluded-but-nearby NPCs stay, matching "available to speak with").
    // We resolve each formID locally only to tag sleep state.
    struct SceneActor
    {
        std::uint32_t formID = 0;
        std::string   name;       // raw (unescaped) display name from SkyrimNet
        float         distUnits = 0.0f;
        std::string   uuid;       // SkyrimNet actor UUID (used to match the "Pinned" group)
    };
    std::mutex               g_sceneMutex;
    std::vector<SceneActor>  g_scene;            // guarded by g_sceneMutex
    std::atomic<bool>        g_sceneValid{false};  // a SkyrimNet fetch has succeeded at least once
    std::atomic<std::uint64_t> g_lastSceneOk{0};   // GetTickCount64() of the last successful nearby-actors fetch
                                                   // (0 = never); drives the "stale list" flag in BuildNpcJson

    // SkyrimNet virtual entities (Game Master / Narrator / custom registered ones). Non-physical
    // speakers addressed by entity UUID (not Actor). We enumerate the ENABLED ones from
    // /config?api=get&name=VirtualEntities and push them as a separate "virtual" array so the panel
    // can offer them as speaker/target for speech/think/prompt only. Refreshed alongside the cast.
    struct VirtualEnt
    {
        std::string entityName;   // unique id ("System Voice")
        std::string displayName;  // shown name ("System") -- also what GetEntityDisplayNameByUUID returns
        std::string voiceId;
        std::string mode;         // conversationMode
        std::string uuid;         // real 64-bit entity UUID (from /characters), for *ByUUID addressing
    };
    std::mutex               g_virtualMutex;
    std::vector<VirtualEnt>  g_virtual;           // ENABLED virtual entities, guarded by g_virtualMutex
    // entityName -> real entity UUID, resolved once from /characters?api=list (that list is ~750 KB and
    // the UUIDs are stable for the session, so we cache rather than refetch each poll).
    std::mutex                                       g_veUuidMutex;
    std::vector<std::pair<std::string, std::string>> g_veUuidCache;
    std::mutex               g_pinnedMutex;
    std::vector<std::string> g_pinnedUuids;       // UUIDs in SkyrimNet's "Pinned" group; guarded by g_pinnedMutex

    constexpr const char* EVT_COMMAND = "PW_PrismaCommand";  // DLL -> Papyrus (action payload)
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

    // Minimal numeric extractor: returns the number after "key": in a JSON blob,
    // searching from `from`. No JSON dep -- SkyrimNet's /config payload is large but we
    // only need three interaction fields. Returns `fallback` if absent/unparseable.
    float JsonNum(const std::string& j, const char* key, size_t from, float fallback)
    {
        const std::string needle = std::string("\"") + key + "\"";
        const size_t k = j.find(needle, from);
        if (k == std::string::npos) {
            return fallback;
        }
        size_t c = j.find(':', k + needle.size());
        if (c == std::string::npos) {
            return fallback;
        }
        ++c;
        while (c < j.size() && (j[c] == ' ' || j[c] == '\t' || j[c] == '\n' || j[c] == '\r')) {
            ++c;
        }
        char* end = nullptr;
        const double v = std::strtod(j.c_str() + c, &end);
        if (end == j.c_str() + c) {
            return fallback;
        }
        return static_cast<float>(v);
    }

    // Boolean variant: 1 = true, 0 = false, returns `fallback` if absent. Accepts JSON
    // true/false or 1/0.
    int JsonBool(const std::string& j, const char* key, int fallback, size_t from = 0)
    {
        const std::string needle = std::string("\"") + key + "\"";
        const size_t k = j.find(needle, from);
        if (k == std::string::npos) {
            return fallback;
        }
        size_t c = j.find(':', k + needle.size());
        if (c == std::string::npos) {
            return fallback;
        }
        ++c;
        while (c < j.size() && (j[c] == ' ' || j[c] == '\t' || j[c] == '\n' || j[c] == '\r')) {
            ++c;
        }
        if (c >= j.size()) {
            return fallback;
        }
        const char ch = j[c];
        if (ch == 't' || ch == 'T' || ch == '1') {
            return 1;
        }
        if (ch == 'f' || ch == 'F' || ch == '0') {
            return 0;
        }
        return fallback;
    }

    // String variant: returns the (unescaped) string value after "key": in `j`, or "" if absent.
    // Handles backslash escapes minimally (enough for SkyrimNet actor names).
    std::string JsonStr(const std::string& j, const char* key)
    {
        const std::string needle = std::string("\"") + key + "\"";
        const size_t k = j.find(needle);
        if (k == std::string::npos) {
            return "";
        }
        size_t c = j.find(':', k + needle.size());
        if (c == std::string::npos) {
            return "";
        }
        ++c;
        while (c < j.size() && (j[c] == ' ' || j[c] == '\t' || j[c] == '\n' || j[c] == '\r')) {
            ++c;
        }
        if (c >= j.size() || j[c] != '"') {
            return "";
        }
        ++c;
        std::string out;
        while (c < j.size() && j[c] != '"') {
            if (j[c] == '\\' && c + 1 < j.size()) {
                out += j[c + 1];
                c += 2;
            } else {
                out += j[c];
                ++c;
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
        g_perkTelepathy = dh->LookupForm<RE::BGSPerk>(0x12DD, "SkyrimNet.esp");
        g_perkTelepathyCanon = dh->LookupForm<RE::BGSPerk>(0x12DE, "SkyrimNet.esp");
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

    // ---- SkyrimNet REST API (editable conversation log) ----
    // Read host/port from SkyrimNet's WebServer.yaml so we honour the user's
    // configured port instead of assuming 8080. Tiny line parser -- no YAML dep.
    void ReadWebServerConfig()
    {
        std::ifstream f("Data\\SKSE\\Plugins\\SkyrimNet\\config\\WebServer.yaml");
        if (!f) {
            return;
        }
        std::string line;
        while (std::getline(f, line)) {
            const auto colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            auto trim = [](std::string s) {
                const auto b = s.find_first_not_of(" \t\r\n");
                const auto e = s.find_last_not_of(" \t\r\n");
                return (b == std::string::npos) ? std::string{} : s.substr(b, e - b + 1);
            };
            const std::string key = trim(line.substr(0, colon));
            const std::string val = trim(line.substr(colon + 1));
            if (key == "host" && !val.empty()) {
                g_apiHost = val;
            } else if (key == "port" && !val.empty()) {
                try {
                    g_apiPort = std::stoi(val);
                } catch (...) {
                }
            }
        }
        logger::info("SkyrimNet API base: http://{}:{}", g_apiHost, g_apiPort);
    }

    // Build the per-NPC action-menu config the panel renders. Read every *.json fragment in
    // Data/SKSE/Plugins/SNPlaywright/buttons/ (MO2/VFS-merged, so ANY mod can drop its own
    // fragment there to add a button -- same extensibility pattern as SkyrimNet's prompt
    // submodules) and concatenate them, in filename order, into one JSON array-of-arrays for
    // the view to flatten. We deliberately do NOT parse the JSON here -- each fragment is a
    // JSON array of button objects; a malformed one just makes the view fall back to its
    // built-in default menu. Which fragments exist is how the FOMOD toggles the sleep buttons
    // (Full ships 20_sleep.json; Promptless omits it). Returns "" when the folder is absent,
    // so the view keeps its built-in default (sleep buttons included) on older installs.
    std::string ReadMenuFragments()
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path dir = "Data\\SKSE\\Plugins\\SNPlaywright\\buttons";
        if (!fs::is_directory(dir, ec)) {
            return "";
        }
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".json") {
                files.push_back(entry.path());
            }
        }
        if (files.empty()) {
            return "";
        }
        std::sort(files.begin(), files.end());  // deterministic load order by filename

        std::string out = "[";
        bool first = true;
        for (const auto& p : files) {
            std::ifstream f(p, std::ios::binary);
            if (!f) {
                continue;
            }
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            const auto b = content.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) {
                continue;  // blank file
            }
            const auto e = content.find_last_not_of(" \t\r\n");
            content = content.substr(b, e - b + 1);
            if (!first) {
                out += ",";
            }
            out += content;  // each fragment is itself a JSON array -> array-of-arrays
            first = false;
        }
        out += "]";
        return first ? std::string{} : out;  // all-blank -> "" -> view keeps its default
    }

    // Scenario files for the script sequencer. Read every *.json in
    // Data/SKSE/Plugins/SNPlaywright/scenarios/ (MO2/VFS-merged, hand-authored or LLM-generated)
    // and wrap each in a {file,body} envelope -- [{"file":"<stem>","body":{...scenario...}}, ...] --
    // so the view knows which file to overwrite/delete when editing. body is the file's raw JSON
    // spliced inline (it's already JSON, no escaping). Same disk pattern as ReadMenuFragments: we
    // do NOT parse the body here, so a malformed file just makes the view skip that entry. "" when
    // the folder is absent or all-blank, so the view shows an empty launcher rather than erroring.
    std::string ReadScenarioFiles()
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path dir = "Data\\SKSE\\Plugins\\SNPlaywright\\scenarios";
        if (!fs::is_directory(dir, ec)) {
            return "";
        }
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".json") {
                files.push_back(entry.path());
            }
        }
        if (files.empty()) {
            return "";
        }
        std::sort(files.begin(), files.end());  // deterministic order by filename

        std::string out = "[";
        bool first = true;
        for (const auto& p : files) {
            std::ifstream f(p, std::ios::binary);
            if (!f) {
                continue;
            }
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            const auto b = content.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) {
                continue;  // blank file
            }
            const auto e = content.find_last_not_of(" \t\r\n");
            content = content.substr(b, e - b + 1);
            if (!first) {
                out += ",";
            }
            out += "{\"file\":\"" + JsonEsc(p.stem().string().c_str()) + "\",\"body\":" + content + "}";
            first = false;
        }
        out += "]";
        return first ? std::string{} : out;
    }

    // Keep a scenario filename stem inside the scenarios/ folder: allow only alnum, dash,
    // underscore, space (drops path separators, dots, '..'), then trim. Empty -> reject the write.
    std::string SanitizeStem(const std::string& a_in)
    {
        std::string out;
        for (char c : a_in) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ';
            if (ok) {
                out += c;
            }
        }
        const auto b = out.find_first_not_of(' ');
        if (b == std::string::npos) {
            return "";
        }
        const auto e = out.find_last_not_of(' ');
        return out.substr(b, e - b + 1);
    }

    // Write/delete a scenario file (editor save/delete). Stem is sanitized so the view can never
    // escape the scenarios folder; the body is whatever JSON the editor serialized.
    void WriteScenarioFile(const std::string& a_stem, const std::string& a_json)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const std::string stem = SanitizeStem(a_stem);
        if (stem.empty()) {
            return;
        }
        const fs::path dir = "Data\\SKSE\\Plugins\\SNPlaywright\\scenarios";
        fs::create_directories(dir, ec);
        std::ofstream f(dir / (stem + ".json"), std::ios::binary | std::ios::trunc);
        if (f) {
            f << a_json;
        }
    }

    void DeleteScenarioFile(const std::string& a_stem)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const std::string stem = SanitizeStem(a_stem);
        if (!stem.empty()) {
            fs::remove(fs::path("Data\\SKSE\\Plugins\\SNPlaywright\\scenarios") / (stem + ".json"), ec);
        }
    }

    // Auto-discover the faction roster: scan the raw fragment JSON for "faction:<token>" tokens
    // (inside showWhen strings) and resolve each to a live Faction. A token is either a
    // "<formid>~<plugin>" pair (e.g. "0x1BDB1~Skyrim.esm" — always resolvable, no dependency) or a
    // bare EditorID (e.g. "DarkBrotherhood" — resolved at runtime, needs EditorIDs to be retained).
    // We scan text rather than parse JSON (no dependency); a token runs from just after "faction:"
    // to the next ',' or '"' (the delimiters in a comma-separated showWhen string), which also
    // tolerates spaces in plugin names. Catches "!faction:" too (the '!' precedes "faction:").
    // Called once at view-ready.
    void BuildMenuFactions(const std::string& a_fragments)
    {
        g_menuFactions.clear();
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh || a_fragments.empty()) {
            return;
        }
        const std::string key = "faction:";
        std::size_t pos = 0;
        while ((pos = a_fragments.find(key, pos)) != std::string::npos) {
            const std::size_t start = pos + key.size();
            const std::size_t stop = a_fragments.find_first_of(",\"", start);
            pos = (stop == std::string::npos) ? a_fragments.size() : stop;
            if (stop == std::string::npos) {
                break;
            }
            std::string tok = a_fragments.substr(start, stop - start);
            const auto b = tok.find_first_not_of(" \t");
            const auto e = tok.find_last_not_of(" \t");
            if (b == std::string::npos) {
                continue;
            }
            tok = tok.substr(b, e - b + 1);
            // dedupe
            bool dup = false;
            for (const auto& mf : g_menuFactions) {
                if (mf.first == tok) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            RE::TESFaction* fac = nullptr;
            const auto tilde = tok.find('~');
            if (tilde == std::string::npos) {
                // Bare EditorID form, e.g. "PlayerFollowerFaction". Needs runtime EditorIDs to be
                // retained (powerofthree's Tweaks "Load EditorIDs", or Native EditorID Fix);
                // resolves to null (button stays hidden) when they aren't.
                fac = RE::TESForm::LookupByEditorID<RE::TESFaction>(tok);
            } else {
                // FormID~Plugin form, e.g. "0x1BDB1~Skyrim.esm". Always available, no dependency.
                RE::FormID localId = 0;
                try {
                    localId = static_cast<RE::FormID>(std::stoul(tok.substr(0, tilde), nullptr, 16));
                } catch (...) {
                    continue;
                }
                const std::string plugin = tok.substr(tilde + 1);
                fac = dh->LookupForm<RE::TESFaction>(localId, plugin);
            }
            g_menuFactions.emplace_back(tok, fac);  // keep even if null so we log the miss once
            logger::info("Menu faction '{}' -> {}", tok, fac ? "resolved" : "NOT FOUND");
        }
    }

    // JSON array of the menu-faction tokens this actor is currently in (for npc.fx). "[]" when
    // no factions are tracked or the actor is in none.
    std::string BuildFactionTags(RE::Actor* a_actor)
    {
        if (g_menuFactions.empty() || !a_actor) {
            return "[]";
        }
        std::string out = "[";
        bool first = true;
        for (const auto& [tok, fac] : g_menuFactions) {
            if (fac && a_actor->IsInFaction(fac)) {
                out += first ? "\"" : ",\"";
                out += JsonEsc(tok.c_str());
                out += "\"";
                first = false;
            }
        }
        out += "]";
        return out;
    }

    // Blocking WinHTTP request (call off the main thread). Returns the response
    // body, or "" on failure.
    std::string HttpRequest(const wchar_t* a_verb, const std::string& a_path, const std::string& a_body)
    {
        std::string result;
        const std::wstring whost(g_apiHost.begin(), g_apiHost.end());
        const std::wstring wpath(a_path.begin(), a_path.end());  // ASCII paths only

        HINTERNET hSession = WinHttpOpen(L"SNPlaywright/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
            return result;
        }
        if (HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(),
                                                static_cast<INTERNET_PORT>(g_apiPort), 0)) {
            if (HINTERNET hRequest = WinHttpOpenRequest(hConnect, a_verb, wpath.c_str(), nullptr,
                                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)) {
                // SkyrimNet content-negotiates: without Accept: application/json it
                // serves the HTML dashboard instead of JSON.
                LPCWSTR headers = L"Accept: application/json\r\nContent-Type: application/json\r\n";
                const BOOL sent = WinHttpSendRequest(
                    hRequest, headers, static_cast<DWORD>(-1L),
                    a_body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(a_body.data()),
                    static_cast<DWORD>(a_body.size()), static_cast<DWORD>(a_body.size()), 0);
                if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
                    DWORD avail = 0;
                    do {
                        avail = 0;
                        if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) {
                            break;
                        }
                        std::string chunk(avail, '\0');
                        DWORD read = 0;
                        if (WinHttpReadData(hRequest, chunk.data(), avail, &read) && read) {
                            result.append(chunk.data(), read);
                        }
                    } while (avail > 0);
                }
                WinHttpCloseHandle(hRequest);
            }
            WinHttpCloseHandle(hConnect);
        }
        WinHttpCloseHandle(hSession);
        return result;
    }

    constexpr const char* LOG_LIST_PATH = "/events?api=list&limit=200&offset=0";

    void PushLog(std::string a_json)
    {
        logger::info("log fetch: {} bytes", a_json.size());
        SKSE::GetTaskInterface()->AddTask([a_json]() {
            if (g_prisma && g_viewReady.load() && g_prisma->IsValid(g_view)) {
                g_prisma->InteropCall(g_view, "pwSetLog", a_json.c_str());
            }
        });
    }

    void FetchLogAsync()
    {
        std::thread([]() { PushLog(HttpRequest(L"GET", LOG_LIST_PATH, "")); }).detach();
    }

    void EditEventAsync(std::string a_id, std::string a_type, std::string a_text)
    {
        std::thread([a_id, a_type, a_text]() {
            // SkyrimNet's /events?api=update validates the whole event like a create:
            // it REQUIRES a non-empty "type" and wants "data" as a plain STRING (an
            // object is rejected). This collapses the event to string data + drops
            // formatted_text -- the same shape SkyrimNet's own edit produces. id is
            // numeric in the store, so send it unquoted.
            std::string body = "{\"id\":" + a_id +
                ",\"type\":\"" + JsonEsc(a_type.c_str()) + "\"" +
                ",\"data\":\"" + JsonEsc(a_text.c_str()) + "\"}";
            HttpRequest(L"PUT", "/events?api=update", body);
            PushLog(HttpRequest(L"GET", LOG_LIST_PATH, ""));  // refresh after edit
        }).detach();
    }

    void DeleteEventAsync(std::string a_id)
    {
        std::thread([a_id]() {
            HttpRequest(L"DELETE", "/events?api=delete&id=" + a_id, "");
            PushLog(HttpRequest(L"GET", LOG_LIST_PATH, ""));  // refresh after delete
        }).detach();
    }

    // --- Action launcher --------------------------------------------------------
    // The SkyrimNet action registry is exposed over the SAME local REST API the log
    // uses, so we reuse HttpRequest + the main-thread InteropCall push. The view
    // renders the param form and fires execution back through pwCommand ->
    // PW_Controller -> SkyrimNetApi.ExecuteAction (no HTTP execute endpoint exists).
    void PushInterop(std::string a_fn, std::string a_json)
    {
        SKSE::GetTaskInterface()->AddTask([a_fn, a_json]() {
            if (g_prisma && g_viewReady.load() && g_prisma->IsValid(g_view)) {
                g_prisma->InteropCall(g_view, a_fn.c_str(), a_json.c_str());
            }
        });
    }

    void FetchActionsAsync()
    {
        std::thread([]() {
            PushInterop("pwSetActions", HttpRequest(L"GET", "/actions?api=actions", ""));
        }).detach();
    }

    // Eligibility for one originator. The view passes its cast token (formid/"player")
    // plus the SkyrimNet UUID; we echo the token back (tab-separated) so the view can
    // route the result to the right window. PROBE-PENDING: confirm the exact query
    // param (uuid vs formid) and the response shape against a live /actions?api=eligibility.
    void FetchEligibleAsync(std::string a_token, std::string a_uuid)
    {
        std::thread([a_token, a_uuid]() {
            std::string path = "/actions?api=eligibility";
            if (!a_uuid.empty()) {
                path += "&uuid=" + a_uuid;
            }
            const std::string resp = HttpRequest(L"GET", path, "");
            PushInterop("pwSetEligible", a_token + "\t" + resp);
        }).detach();
    }

    // Many action descriptions are raw Jinja ({{render_template("helpers/...")}}). The
    // /actions API returns them unrendered; we fetch the referenced template's source via
    // SkyrimNet's prompts API so the view can strip the Jinja to readable prose. (Correct
    // per-actor rendering would need SkyrimNet's template engine, which we can't run.)
    void FetchTemplateAsync(std::string a_path)
    {
        std::thread([a_path]() {
            const std::string resp = HttpRequest(L"GET", "/prompts?api=get&path=" + a_path + ".prompt", "");
            PushInterop("pwSetTemplate", a_path + "\t" + resp);
        }).detach();
    }

    struct NpcEntry
    {
        std::string name;
        std::string id;     // "0xXXXXXXXX"
        int         dist;   // meters
        const char* state;  // "awake" | "asleep" | "sleeptalk"
        bool        follower = false;  // the player's teammate/follower
        bool        pinned = false;    // member of SkyrimNet's "Pinned" group
        std::string factions = "[]";   // JSON array of matched "faction:..." showWhen tokens (npc.fx)
        std::string uuid = "";         // SkyrimNet entity UUID (for /actions?api=eligibility); "" if unknown
        bool        raw = false;       // came from the local engine enumeration, NOT SkyrimNet's cast
                                       // (all-actors mode: SkyrimNet-ineligible/unprofiled -- UI tints these)
    };

    // Refresh SkyrimNet's "Pinned" NPC group (dashboard pins) into g_pinnedUuids. Two cheap localhost
    // queries: /npc-groups?api=list to find the group named "Pinned" -> its id, then ?api=members&id=<id>
    // for the member UUIDs. BuildNpcJson matches these against each cast actor's uuid to flag pinned.
    // Runs OFF the main thread (HTTP). On any HTTP failure we KEEP the last good set (no blink).
    void FetchPinnedBlocking()
    {
        const std::string list = HttpRequest(L"GET", "/npc-groups?api=list", "");
        if (list.empty()) {
            return;  // server off -> keep last good pinned set
        }
        int groupId = -1;
        {
            const size_t gp = list.find("\"groups\"");
            const size_t lb = (gp != std::string::npos) ? list.find('[', gp) : std::string::npos;
            const size_t rb = (lb != std::string::npos) ? list.find(']', lb) : std::string::npos;
            size_t pos = (lb != std::string::npos) ? lb + 1 : std::string::npos;
            while (pos != std::string::npos && pos < rb) {
                const size_t os = list.find('{', pos);
                if (os == std::string::npos || os > rb) {
                    break;
                }
                const size_t oe = list.find('}', os);  // group objects are flat
                if (oe == std::string::npos || oe > rb) {
                    break;
                }
                const std::string obj = list.substr(os, oe - os + 1);
                pos = oe + 1;
                if (JsonStr(obj, "name") == "Pinned") {
                    groupId = static_cast<int>(JsonNum(obj, "id", 0, -1.0f));
                    break;
                }
            }
        }
        std::vector<std::string> pinned;  // empty (with a successful list fetch) = genuinely nothing pinned
        if (groupId >= 0) {
            const std::string mem =
                HttpRequest(L"GET", "/npc-groups?api=members&id=" + std::to_string(groupId), "");
            if (mem.empty()) {
                return;  // transient members-fetch failure -> keep last good set
            }
            const size_t mp = mem.find("\"members\"");
            const size_t lb = (mp != std::string::npos) ? mem.find('[', mp) : std::string::npos;
            const size_t rb = (lb != std::string::npos) ? mem.find(']', lb) : std::string::npos;
            size_t pos = (lb != std::string::npos) ? lb + 1 : std::string::npos;
            while (pos != std::string::npos && pos < rb) {
                const size_t os = mem.find('{', pos);
                if (os == std::string::npos || os > rb) {
                    break;
                }
                const size_t oe = mem.find('}', os);  // member objects are flat
                if (oe == std::string::npos || oe > rb) {
                    break;
                }
                const std::string obj = mem.substr(os, oe - os + 1);
                pos = oe + 1;
                std::string uuid = JsonStr(obj, "uuid");
                if (!uuid.empty()) {
                    pinned.push_back(std::move(uuid));
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_pinnedMutex);
            g_pinnedUuids.swap(pinned);
        }
    }

    void FetchVirtualBlocking();                       // defined just below FetchSceneBlocking (called from it)
    void ResolveVeUuids(std::vector<VirtualEnt>&);     // fills VE real UUIDs (defined below FetchVirtualBlocking)

    // Split the JSON array that follows "key" into its top-level {...} object substrings. Brace- AND
    // quote-aware: a '}' or ']' inside a string value, or a nested object, no longer desyncs the walk
    // (the old find('}') / find(']') scan assumed every object was flat and would silently drop the
    // rest of the array the moment one wasn't). Returns [] if the key/array isn't present.
    std::vector<std::string> JsonObjectsInArray(const std::string& s, const char* key)
    {
        std::vector<std::string> out;
        const size_t kp = s.find(key);
        if (kp == std::string::npos) {
            return out;
        }
        const size_t lb = s.find('[', kp);
        if (lb == std::string::npos) {
            return out;
        }
        const size_t n = s.size();
        size_t i = lb + 1;
        while (i < n) {
            while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == ',')) {
                ++i;
            }
            if (i >= n || s[i] == ']') {
                break;  // end of array
            }
            if (s[i] != '{') {
                ++i;
                continue;  // unexpected token between objects -- skip it
            }
            const size_t start = i;
            int depth = 0;
            bool inStr = false, esc = false;
            for (; i < n; ++i) {
                const char c = s[i];
                if (inStr) {
                    if (esc) {
                        esc = false;
                    } else if (c == '\\') {
                        esc = true;
                    } else if (c == '"') {
                        inStr = false;
                    }
                } else if (c == '"') {
                    inStr = true;
                } else if (c == '{') {
                    ++depth;
                } else if (c == '}') {
                    if (--depth == 0) {
                        out.push_back(s.substr(start, i - start + 1));
                        ++i;
                        break;
                    }
                }
            }
            if (depth != 0) {
                break;  // truncated object -> stop rather than emit garbage
            }
        }
        return out;
    }

    // Pull SkyrimNet's nearby/addressable cast and cache it in g_scene. Runs OFF the main thread (HTTP).
    //
    // /game-data?api=nearby-actors is a lightweight, reliable data query (unlike the render-template
    // endpoint, which is heavy and fails under 1 Hz polling). It returns SkyrimNet's in-scene set --
    // the awareness radius (we pass maxDistance * nearbyNPCAwarenessMultiplier, the value
    // get_nearby_npc_list defaults to) + ActorFilter, with NO line-of-sight filtering, so
    // occluded-but-nearby NPCs stay listed. We drop the player (isPlayer) -- it's a separate selectable
    // row. Each actor carries formID (hex) + name + distance; we keep formID + name (distance is
    // recomputed live in BuildNpcJson).
    //
    // On any HTTP failure we KEEP the last good snapshot (no blink); g_sceneValid only goes true.
    void FetchSceneBlocking()
    {
        float r = g_interactRadius.load() * g_awarenessMult.load();
        if (r < 1.0f) {
            r = 1.0f;
        }
        if (r > 10000.0f) {
            r = 10000.0f;  // SkyrimNet hard-caps nearby-actors radius at 10000
        }
        const std::string path =
            "/game-data?api=nearby-actors&radius=" + std::to_string(static_cast<int>(r + 0.5f));
        const std::string resp = HttpRequest(L"GET", path, "");

        if (resp.empty() || resp.find("\"actors\"") == std::string::npos) {
            return;  // server off / bad response -> keep last good list
        }
        std::vector<SceneActor> scene;
        for (const auto& obj : JsonObjectsInArray(resp, "\"actors\"")) {
            if (JsonBool(obj, "isPlayer", 0) == 1) {
                continue;  // player is rendered separately
            }
            const std::string fid = JsonStr(obj, "formID");
            if (fid.empty()) {
                continue;
            }
            const auto formID = static_cast<std::uint32_t>(std::strtoul(fid.c_str(), nullptr, 16));  // hex
            if (formID == 0) {
                continue;
            }
            scene.push_back(SceneActor{ formID, JsonStr(obj, "name"), JsonNum(obj, "distance", 0, 0.0f), JsonStr(obj, "uuid") });
        }
        {
            std::lock_guard<std::mutex> lock(g_sceneMutex);
            g_scene.swap(scene);
        }
        g_sceneValid.store(true);
        g_lastSceneOk.store(GetTickCount64());  // stamp success -> BuildNpcJson can flag a stale (frozen) list
        FetchPinnedBlocking();  // refresh SkyrimNet's "Pinned" group alongside the cast (cheap localhost)
        FetchVirtualBlocking(); // refresh enabled virtual entities alongside the cast (cheap localhost)
    }

    // Pull SkyrimNet's ENABLED virtual entities and cache them in g_virtual. Runs OFF the main thread.
    // /config?api=get&name=VirtualEntities returns {data:{...,entities:[{entityName,displayName,enabled,
    // voiceId,conversationMode},...]}}. We keep only enabled==true (respect disabled). The objects are
    // flat (no nested braces), so we walk them the same way FetchSceneBlocking walks actors. On any HTTP
    // failure we keep the last good list (no blink).
    void FetchVirtualBlocking()
    {
        const std::string resp = HttpRequest(L"GET", "/config?api=get&name=VirtualEntities", "");
        if (resp.empty() || resp.find("\"entities\"") == std::string::npos) {
            return;  // server off / bad response -> keep last good list
        }
        std::vector<VirtualEnt> ents;
        for (const auto& obj : JsonObjectsInArray(resp, "\"entities\"")) {
            if (JsonBool(obj, "enabled", 0) != 1) {
                continue;  // respect disabled virtual entities
            }
            std::string en = JsonStr(obj, "entityName");
            std::string dn = JsonStr(obj, "displayName");
            if (dn.empty()) {
                dn = en;
            }
            if (dn.empty()) {
                continue;
            }
            ents.push_back(VirtualEnt{ en, dn, JsonStr(obj, "voiceId"), JsonStr(obj, "conversationMode"), "" });
        }
        ResolveVeUuids(ents);   // fill in each VE's real entity UUID (cached; one /characters fetch)
        {
            std::lock_guard<std::mutex> lock(g_virtualMutex);
            g_virtual.swap(ents);
        }
    }

    // Resolve each virtual entity's real 64-bit UUID by its registration name. Virtual NPCs aren't in
    // the 0x1000000 range the get_virtual_npc_list decorator's EXAMPLE implied -- they carry full hashed
    // UUIDs (e.g. "Buddy" -> 6D247B5D915BC6C7). /characters?api=list exposes them as actorName/actorUUID
    // pairs. We cache (the list is ~750 KB) and only fetch when an entity's UUID isn't cached yet.
    std::string CachedVeUuid(const std::string& name)
    {
        std::lock_guard<std::mutex> lk(g_veUuidMutex);
        for (const auto& kv : g_veUuidCache) {
            if (kv.first == name) {
                return kv.second;
            }
        }
        return "";
    }
    void ResolveVeUuids(std::vector<VirtualEnt>& ents)
    {
        bool anyMissing = false;
        for (auto& e : ents) {
            e.uuid = CachedVeUuid(e.entityName);
            if (e.uuid.empty() && !e.entityName.empty()) {
                anyMissing = true;
            }
        }
        if (!anyMissing) {
            return;  // every enabled VE already resolved -> skip the heavy fetch
        }
        const std::string resp = HttpRequest(L"GET", "/characters?api=list", "");
        if (resp.empty()) {
            return;
        }
        for (auto& e : ents) {
            if (!e.uuid.empty() || e.entityName.empty()) {
                continue;
            }
            // Find this entity's object by its actorName, then the actorUUID that follows it.
            const std::string needle = "\"actorName\":\"" + e.entityName + "\"";
            const size_t p = resp.find(needle);
            if (p == std::string::npos) {
                continue;
            }
            const std::string uk = "\"actorUUID\":\"";
            const size_t u = resp.find(uk, p);
            if (u == std::string::npos) {
                continue;
            }
            const size_t s = u + uk.size();
            const size_t end = resp.find('"', s);
            if (end == std::string::npos) {
                continue;
            }
            const std::string uuid = resp.substr(s, end - s);
            if (uuid.empty()) {
                continue;
            }
            e.uuid = uuid;
            std::lock_guard<std::mutex> lk(g_veUuidMutex);
            g_veUuidCache.emplace_back(e.entityName, uuid);
        }
    }

    // Build the {"directorMode":bool,"npcs":[...]} payload. Primary source is SkyrimNet's scene snapshot
    // (g_scene, filled by FetchSceneBlocking) so the list matches SkyrimNet exactly; if SkyrimNet's web
    // server has never answered, fall back to a local enumeration (distance + optional occlusion raycast).
    std::string BuildNpcJson()
    {
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) {
            return "{\"directorMode\":false,\"npcs\":[]}";
        }

        const float maxUnits = g_interactRadius.load();  // mirrors SkyrimNet's live interaction range
        const bool  sceneOk = g_sceneValid.load();
        const bool  allActors = g_allActors.load();
        std::vector<NpcEntry> entries;
        std::vector<std::uint32_t> seen;  // formIDs already listed -- dedup the local union (cast is small, linear is fine)
        const RE::NiPoint3 ppos = pc->GetPosition();

        if (sceneOk) {
            // PRIMARY: SkyrimNet decides membership (its nearby/addressable cast). We resolve each formID
            // to tag sleep state AND to measure the live distance ourselves from the actor's real position
            // (fresher than the fetch-time snapshot). sa.distUnits is only a backstop if the actor is gone.
            std::vector<std::string> pinnedUuids;
            {
                std::lock_guard<std::mutex> plk(g_pinnedMutex);
                pinnedUuids = g_pinnedUuids;
            }
            std::lock_guard<std::mutex> lock(g_sceneMutex);
            for (const auto& sa : g_scene) {
                const char* st = "awake";
                float du = sa.distUnits;
                bool follower = false;
                bool pinned = false;
                std::string fx = "[]";
                for (const auto& pu : pinnedUuids) {
                    if (pu == sa.uuid) {
                        pinned = true;
                        break;
                    }
                }
                if (auto* a = RE::TESForm::LookupByID<RE::Actor>(sa.formID)) {
                    du = ppos.GetDistance(a->GetPosition());
                    if (g_facAsleep && a->IsInFaction(g_facAsleep)) {
                        st = "asleep";
                    } else if (g_facSleeptalk && a->IsInFaction(g_facSleeptalk)) {
                        st = "sleeptalk";
                    }
                    follower = a->IsPlayerTeammate();
                    fx = BuildFactionTags(a);
                }
                char idbuf[16];
                std::snprintf(idbuf, sizeof(idbuf), "0x%08X", sa.formID);
                entries.push_back(NpcEntry{
                    JsonEsc(sa.name.c_str()),
                    idbuf,
                    static_cast<int>(du / UNITS_PER_M + 0.5f),
                    st,
                    follower,
                    pinned,
                    fx,
                    sa.uuid });
                seen.push_back(sa.formID);
            }
        }

        // LOCAL UNION: in all-actors mode (or whenever SkyrimNet has never answered) walk the engine's
        // own loaded-actor list and add anyone in range we don't already have. These are the actors
        // SkyrimNet filtered out (no voice type) or hasn't profiled into nearby-actors yet -- exactly the
        // generics users report missing. Marked raw=true when augmenting a real SkyrimNet list so the UI
        // can tint them; a pure fallback (SkyrimNet offline) isn't "extra", so it stays unmarked.
        if (allActors || !sceneOk) {
            if (auto* lists = RE::ProcessLists::GetSingleton()) {
                float lr = maxUnits * g_awarenessMult.load();  // match SkyrimNet's awareness radius (maxDistance * mult)
                if (lr < 1.0f) {
                    lr = 1.0f;
                }
                for (auto& handle : lists->highActorHandles) {
                    auto ptr = handle.get();
                    RE::Actor* a = ptr.get();
                    if (!a || a == pc || a->IsDead() || a->IsDisabled() || !a->Is3DLoaded()) {
                        continue;
                    }
                    const std::uint32_t fid = a->GetFormID();
                    bool dup = false;
                    for (auto f : seen) {
                        if (f == fid) {
                            dup = true;
                            break;
                        }
                    }
                    if (dup) {
                        continue;  // already in SkyrimNet's list -> keep its richer metadata (uuid/pinned)
                    }
                    const char* dn = a->GetDisplayFullName();
                    if (!dn || !dn[0]) {
                        continue;  // named actors only -- generics have names; blank-name rows aren't addressable
                    }
                    const float du = ppos.GetDistance(a->GetPosition());
                    if (du > lr) {
                        continue;
                    }
                    const char* st = "awake";
                    if (g_facAsleep && a->IsInFaction(g_facAsleep)) {
                        st = "asleep";
                    } else if (g_facSleeptalk && a->IsInFaction(g_facSleeptalk)) {
                        st = "sleeptalk";
                    }
                    char idbuf[16];
                    std::snprintf(idbuf, sizeof(idbuf), "0x%08X", fid);
                    NpcEntry ne;
                    ne.name = JsonEsc(dn);
                    ne.id = idbuf;
                    ne.dist = static_cast<int>(du / UNITS_PER_M + 0.5f);
                    ne.state = st;
                    ne.follower = a->IsPlayerTeammate();
                    ne.pinned = false;
                    ne.factions = BuildFactionTags(a);
                    ne.uuid = "";
                    ne.raw = sceneOk;  // "extra" beyond SkyrimNet's list -> raw; pure offline fallback -> not
                    entries.push_back(std::move(ne));
                    seen.push_back(fid);
                }
            }
        }

        std::sort(entries.begin(), entries.end(),
                  [](const NpcEntry& l, const NpcEntry& r) { return l.dist < r.dist; });
        const size_t cap = g_maxCast > 0 ? static_cast<size_t>(g_maxCast) : 50;
        if (entries.size() > cap) {
            entries.resize(cap);
        }

        const bool director = g_facBlacklist && pc->IsInFaction(g_facBlacklist);

        std::string json = "{\"directorMode\":";
        json += director ? "true" : "false";
        json += ",\"pauseGame\":";
        json += g_pauseGame ? "true" : "false";
        json += ",\"whisper\":";
        json += g_whisperOn.load() ? "true" : "false";
        json += ",\"continuous\":";
        json += g_continuousOn.load() ? "true" : "false";
        json += ",\"gamemaster\":";
        json += g_gamemasterOn.load() ? "true" : "false";
        json += ",\"npcReactions\":";
        json += g_npcReactionsOn.load() ? "true" : "false";
        // Telepathy: the player perceives NPC thoughts only with a SkyrimNet Telepathy perk
        // (basic or canonical). The view uses this to show/hide npc_thoughts in the log.
        json += ",\"telepathy\":";
        json += ((g_perkTelepathy && pc->HasPerk(g_perkTelepathy)) ||
                 (g_perkTelepathyCanon && pc->HasPerk(g_perkTelepathyCanon)))
                    ? "true"
                    : "false";
        json += ",\"radius\":";
        json += std::to_string(static_cast<int>(maxUnits / UNITS_PER_M + 0.5f));
        json += ",\"allActors\":";
        json += allActors ? "true" : "false";
        // Stale = SkyrimNet answered before but the 1 Hz poll has now been failing for >4 s, so the cast
        // is frozen on an old snapshot. Suppressed in all-actors mode (the local union keeps it live).
        json += ",\"stale\":";
        {
            const std::uint64_t lastOk = g_lastSceneOk.load();
            const bool stale = sceneOk && !allActors && lastOk != 0 && (GetTickCount64() - lastOk > 4000);
            json += stale ? "true" : "false";
        }
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
            json += " (you)\",\"id\":\"player\",\"uuid\":\"\",\"player\":true,\"state\":\"";
            json += pst;
            json += "\",\"fx\":";
            json += BuildFactionTags(pc);
            json += "}";
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
            json += "\",\"follower\":";
            json += e.follower ? "true" : "false";
            json += ",\"pinned\":";
            json += e.pinned ? "true" : "false";
            json += ",\"fx\":";
            json += e.factions;
            json += ",\"uuid\":\"";
            json += e.uuid;
            json += "\",\"raw\":";
            json += e.raw ? "true" : "false";
            json += "}";
        }
        json += "],\"virtual\":[";
        {
            std::lock_guard<std::mutex> lock(g_virtualMutex);
            bool firstVe = true;
            for (const auto& v : g_virtual) {
                if (!firstVe) {
                    json += ",";
                }
                firstVe = false;
                json += "{\"name\":\"";
                json += JsonEsc(v.displayName.c_str());
                json += "\",\"entityName\":\"";
                json += JsonEsc(v.entityName.c_str());
                json += "\",\"voiceId\":\"";
                json += JsonEsc(v.voiceId.c_str());
                json += "\",\"mode\":\"";
                json += JsonEsc(v.mode.c_str());
                json += "\",\"uuid\":\"";
                json += JsonEsc(v.uuid.c_str());
                json += "\"}";
            }
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

    // Background poll: while the panel is open, re-pull SkyrimNet's nearby list every second so actors
    // that walk into/out of the scene while you reposition (esp. in RMB look mode) get added/removed. The
    // HTTP fetch runs here (off the main thread); the resulting push is gated on !GameIsPaused so a frozen
    // world doesn't disturb the panel. The view ignores no-op pushes (same cast set) so an open menu survives.
    void StartActorPoll()
    {
        std::thread([]() {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (!g_panelOpen.load() || !g_viewReady.load()) {
                    continue;
                }
                FetchSceneBlocking();  // refresh SkyrimNet's nearby list (blocking HTTP, off-thread)
                SKSE::GetTaskInterface()->AddTask([]() {
                    auto* ui = RE::UI::GetSingleton();
                    if (!ui || ui->GameIsPaused()) {
                        return;  // frozen world -> don't disturb the panel
                    }
                    if (g_prisma && g_viewReady.load() && g_prisma->IsValid(g_view)) {
                        const std::string json = BuildNpcJson();
                        g_prisma->InteropCall(g_view, "pwSetData", json.c_str());
                    }
                });
            }
        }).detach();
    }

    // Mirror SkyrimNet's whisper mode + interaction radius so the cast list matches its
    // scene. Two reads, because they live in different places:
    //   * /config?api=get&name=game  -> the STATIC radii (normalMaxDistance, whisperMaxDistance).
    //   * /?api=gamemaster-status     -> the LIVE on/off (gamemaster.whisper_mode).
    // The runtime whisper toggle is NOT reflected in /config (config.maxDistance stays at the
    // normal value), which is why reading it alone made the pill stick at "off" -- SkyrimNet's
    // own dashboard reads whisper_mode from the gamemaster-status endpoint. The effective range is then
    // whisperMaxDistance when on, else normalMaxDistance (= interaction.maxDistance). We only use this for
    // the radius pill + whisper state; the cast list comes from SkyrimNet's own decorators, which apply
    // the awareness radius (and honour whisper) internally. Runs off the main thread (HTTP), then pulls the
    // scene + PushData()s so the view picks up the fresh radius, whisper pill, and cast list.
    constexpr const char* CONFIG_GAME_PATH = "/config?api=get&name=game";
    constexpr const char* CONFIG_EVENTS_PATH = "/config?api=get&name=Events";
    constexpr const char* GM_STATUS_PATH = "/?api=gamemaster-status";

    void FetchConfigAsync()
    {
        std::thread([]() {
            // (1) Static radii from the game config group.
            const std::string cfg = HttpRequest(L"GET", CONFIG_GAME_PATH, "");
            if (!cfg.empty()) {
                const size_t ip = cfg.find("\"interaction\"");
                const size_t from = (ip == std::string::npos) ? 0 : ip;
                const float normal = JsonNum(cfg, "normalMaxDistance", from, g_normalDist.load());
                const float whisper = JsonNum(cfg, "whisperMaxDistance", from, g_whisperDist.load());
                const float mult = JsonNum(cfg, "nearbyNPCAwarenessMultiplier", from, g_awarenessMult.load());
                if (normal > 0.0f) {
                    g_normalDist.store(normal);
                }
                if (whisper > 0.0f) {
                    g_whisperDist.store(whisper);
                }
                if (mult > 0.0f) {
                    g_awarenessMult.store(mult);
                }
                // (GameMaster on/off is read from the LIVE status endpoint below, not here:
                // gamemaster.enabled in /config is a static master switch that never moves;
                // TriggerToggleGameMaster flips the live AGENT, which surfaces only as
                // agent_enabled in /?api=gamemaster-status -- same as whisper_mode.)
            }
            // (1b) NPC world-event reactions live in the Events config group (global section).
            const std::string ev = HttpRequest(L"GET", CONFIG_EVENTS_PATH, "");
            if (!ev.empty()) {
                const size_t gp = ev.find("\"global\"");
                const int en = JsonBool(ev, "npcReactionsEnabled", -1, (gp == std::string::npos) ? 0 : gp);
                if (en >= 0) {
                    g_npcReactionsOn.store(en == 1);
                }
            }
            // (2) Live whisper + GameMaster-agent on/off -- these runtime toggles only show up
            //     here, not in /config (whisper_mode / agent_enabled in gamemaster-status).
            const std::string gm = HttpRequest(L"GET", GM_STATUS_PATH, "");
            if (!gm.empty()) {
                const int wm = JsonBool(gm, "whisper_mode", -1);
                if (wm >= 0) {
                    g_whisperOn.store(wm == 1);
                }
                const int ae = JsonBool(gm, "agent_enabled", -1);
                if (ae >= 0) {
                    g_gamemasterOn.store(ae == 1);
                }
                const int cm = JsonBool(gm, "continuous_scene_mode", -1);
                if (cm >= 0) {
                    g_continuousOn.store(cm == 1);
                }
            }
            // Effective range follows the live state, using the static radii.
            g_interactRadius.store(g_whisperOn.load() ? g_whisperDist.load() : g_normalDist.load());
            logger::info("interaction: normal={} whisper={} whisper_mode={} -> radius={} | gamemaster={} npcReactions={}",
                         g_normalDist.load(), g_whisperDist.load(),
                         g_whisperOn.load() ? "ON" : "off", g_interactRadius.load(),
                         g_gamemasterOn.load() ? "ON" : "off", g_npcReactionsOn.load() ? "ON" : "off");
            FetchSceneBlocking();  // pull SkyrimNet's nearby/addressable cast
            PushData();
        }).detach();
    }

    // Rewrite interaction.maxDistance's numeric value in a /config game-config JSON blob.
    // Scoped to the interaction object so it never touches spatialAudio.maxDistance.
    bool SetInteractionMaxDistance(std::string& json, int newVal)
    {
        const size_t ip = json.find("\"interaction\"");
        if (ip == std::string::npos) {
            return false;
        }
        const size_t kp = json.find("\"maxDistance\"", ip);
        if (kp == std::string::npos) {
            return false;
        }
        size_t c = json.find(':', kp);
        if (c == std::string::npos) {
            return false;
        }
        ++c;
        while (c < json.size() && (json[c] == ' ' || json[c] == '\t' || json[c] == '\n' || json[c] == '\r')) {
            ++c;
        }
        size_t e = c;
        while (e < json.size() && ((json[e] >= '0' && json[e] <= '9') || json[e] == '.' || json[e] == '-' ||
                                   json[e] == '+' || json[e] == 'e' || json[e] == 'E')) {
            ++e;
        }
        if (e == c) {
            return false;
        }
        json.replace(c, e - c, std::to_string(newVal));
        return true;
    }

    // Toggle SkyrimNet whisper mode by flipping interaction.maxDistance ourselves. SkyrimNet's
    // Papyrus TriggerToggleWhisperMode() no-ops when called from our ModEvent/menu context (works
    // from its own wheel in gameplay, not from here), so we go straight to the config: GET the
    // game group, swap maxDistance between normal/whisper, POST it back. SkyrimNet derives
    // gamemaster.whisper_mode from maxDistance, so the flag + radius both update -- verified live.
    void ToggleWhisperAsync()
    {
        std::thread([]() {
            std::string resp = HttpRequest(L"GET", CONFIG_GAME_PATH, "");
            const size_t ip = resp.find("\"interaction\"");
            if (resp.empty() || ip == std::string::npos) {
                logger::info("whisper toggle: config GET failed");
                PushData();
                return;
            }
            const float normal = JsonNum(resp, "normalMaxDistance", 0, 1600.0f);
            const float whisper = JsonNum(resp, "whisperMaxDistance", 0, 200.0f);
            const float cur = JsonNum(resp, "maxDistance", ip, normal);
            // Currently whisper (near the whisper value)? -> go normal; else -> go whisper.
            const int target = (cur <= (normal + whisper) * 0.5f) ? static_cast<int>(normal)
                                                                  : static_cast<int>(whisper);
            if (SetInteractionMaxDistance(resp, target)) {
                HttpRequest(L"POST", "/config?api=update", resp);
                logger::info("whisper toggle: maxDistance {} -> {}", cur, target);
            }
            FetchConfigAsync();  // re-read live state -> refresh pill + cast radius
        }).detach();
    }

    bool ReadAlwaysRun() { return g_alwaysRun && g_alwaysRun->data.b; }

    // Clear the "stuck in walk mode after the panel closes" latch. The Run modifier (Left Shift, held
    // to open the panel) is a HELD-state input: the engine only flips PlayerControls::data.running when
    // RunHandler processes the press/release transitions. If Shift's RELEASE lands while PrismaUI's focus
    // menu owns input, RunHandler never sees it, so data.running stays latched at "walk". (This is why
    // the old bAlwaysRunByDefault guard never helped -- that INI never actually flips; the DIAG logs
    // proved it stayed true the whole time.) Re-derive data.running from the LIVE key state with the
    // engine's own formula (running = runKeyHeld XOR alwaysRun) -- the same thing Caps-Lock /
    // ToggleRunHandler does. Main thread only, after focus is released, so the keyboard poll reflects the
    // real post-menu key state. Ref: alexoj/FixToggleWalkRun.
    void ResyncRunState()
    {
        auto* pc = RE::PlayerControls::GetSingleton();
        auto* userEvts = RE::UserEvents::GetSingleton();
        auto* idm = RE::BSInputDeviceManager::GetSingleton();
        auto* cmap = RE::ControlMap::GetSingleton();
        if (!pc || !userEvts || !idm || !cmap) {
            return;
        }
        const bool          alwaysRun = ReadAlwaysRun();
        const std::uint32_t runKey = cmap->GetMappedKey(userEvts->run.c_str(), RE::INPUT_DEVICE::kKeyboard);
        bool                runHeld = false;
        if (runKey != RE::ControlMap::kInvalid && runKey < 256) {
            if (auto* kb = idm->GetKeyboard()) {
                // Read the DirectInput key-state buffer directly instead of BSWin32KeyboardDevice::IsPressed:
                // calling that out-of-line method drags in device TUs whose virtuals are unresolved in this
                // CommonLibSSE-NG build (link error). curState[scancode] has 0x80 set while the key is down.
                runHeld = (kb->GetRuntimeData().curState[runKey] & 0x80) != 0;
            }
        }
        const bool running = (runHeld != alwaysRun);  // XOR: with Always Run on, holding Run = walk
        if (pc->data.running != running) {
            logger::info("DIAG resync run/walk: held={} alwaysRun={} -> running={}", runHeld, alwaysRun, running);
            pc->data.running = running;
        }
    }

    // The panel is a PrismaUI focus menu: Focus captures input, Unfocus returns control to gameplay.
    // Focus is a plain call; Unfocus additionally re-syncs the run/walk latch on the next frame (deferred
    // so the keyboard poll has settled), clearing the stuck-walk the menu leaves when it eats the
    // Run-modifier release. Named wrappers so the call sites read intent and stay in one place.
    void GuardedFocus(bool a_pause)
    {
        g_prisma->Focus(g_view, a_pause);
    }
    void GuardedUnfocus()
    {
        const auto v = g_view;  // distinct form so the bulk call-site rewrite skips this real call
        g_prisma->Unfocus(v);
        SKSE::GetTaskInterface()->AddTask([]() { ResyncRunState(); });
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
            // A cycle only fires JS focus/blur echoes when a text field is focused (g_typing):
            // Unfocus blurs it (typing|0) and re-Focus refocuses it (typing|1). When #kc/body is
            // focused (g_typing false) the cycle is silent. Tell OnJsConfig exactly how many echoes
            // to swallow, so it never eats a genuine focus change that merely lands nearby in time.
            g_pendingTypingEchoes.store(g_typing.load() ? 2 : 0);
            GuardedUnfocus();
            GuardedFocus(g_appliedPause);
        });
    }

    void ToggleMenu()
    {
        if (!g_prisma || !g_viewReady.load() || !g_prisma->IsValid(g_view)) {
            return;
        }
        if (g_prisma->HasFocus(g_view)) {
            if (auto* p = RE::PlayerCharacter::GetSingleton()) {
                logger::info("DIAG panel CLOSE run={} walk={} alwaysRun={}",
                             p->IsRunning(), p->IsWalking(), ReadAlwaysRun());
            }
            g_typing = false;  // closing clears the transient typing-pause
            g_panelOpen.store(false);
            g_lookMode.store(false);
            g_appliedPause = false;
            GuardedUnfocus();
            g_prisma->InteropCall(g_view, "pwSetOpen", "0");  // collapse to dot
        } else {
            if (auto* p = RE::PlayerCharacter::GetSingleton()) {
                logger::info("DIAG panel OPEN pause={} run={} walk={} alwaysRun={}", EffectivePause(),
                             p->IsRunning(), p->IsWalking(), ReadAlwaysRun());
            }
            g_panelOpen.store(true);  // panel now owns keyboard (input hook filters non-typing keys)
            g_appliedPause = EffectivePause();
            GuardedFocus(g_appliedPause);
            g_prisma->InteropCall(g_view, "pwSetOpen", "1");  // expand panel
            PushData();
            FetchConfigAsync();  // re-sync radius/whisper (may have changed via SkyrimNet's own hotkey)
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
                } else if (nm == "PW_PrismaStat") {
                    // Scenario sequencer: relay the live speech state (queue|msSinceAudio|ttsFin)
                    // straight to the view so its JS arm->drain pacing loop can advance.
                    PushInterop("pwBeatStat", a_event->strArg.c_str());
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        PrismaModEventSink() = default;
    };

    // Watch for the Main Menu opening (initial title screen OR quit-to-menu) so we can clear in-game
    // state -- otherwise the persistent log keeps floating on the title screen after a quit-to-menu.
    class MenuWatch : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        static MenuWatch* GetSingleton()
        {
            static MenuWatch instance;
            return &instance;
        }
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                              RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (a_event && a_event->opening && a_event->menuName == RE::MainMenu::MENU_NAME) {
                g_inGame.store(false);
                PushInterop("pwSetIngame", "0");
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        MenuWatch() = default;
    };

    void CollapseFromCode()  // close the panel from C++ (main thread)
    {
        SKSE::GetTaskInterface()->AddTask([]() {
            if (g_prisma && g_viewReady.load() && g_prisma->IsValid(g_view)) {
                g_typing = false;
                g_panelOpen.store(false);
                g_lookMode.store(false);
                g_appliedPause = false;
                GuardedUnfocus();
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

    // ---- Input-dispatch hook: steal keys from other consumers while the panel owns input ----
    //
    // A BSTEventSink<InputEvent*> CANNOT block input from other consumers: MenuControls,
    // PlayerControls, AND SKSE's RegisterForKey are all sibling sinks on the single
    // BSInputDeviceManager event source, so no return value gates them. PrismaUI's focus
    // menu only captures keys the CEF page actually consumes (i.e. while you're typing in a
    // field) -- unhandled keys fall through to the game and other mods. To suppress keys
    // while the panel is open but NOT typing, we hook the dispatch call inside
    // BSInputDeviceManager::PollInputDevices and UNLINK keyboard events from the InputEvent
    // linked list before the original dispatcher delivers it -- making them invisible to
    // every consumer at once (RELOCATION_ID(67315,68617)+0x7B; the Wheeler/TDM hook point).
    //
    // Gating: only filter when the panel is expanded/focused AND the text box is NOT focused.
    // While typing, PrismaUI's focus menu already routes keys into the field, so we leave the
    // list untouched (filtering it would break typing, since our detour runs before the menu
    // sink). Escape (DIK 0x01) is always whitelisted so EscInputSink can still close the panel.
    //
    // RMB "look/move" mode: holding right mouse while the panel is open momentarily Unfocuses
    // the view (kept SHOWN) so the game restores mouselook + WASD movement -- reposition for the
    // scene without closing the panel. While in look mode we stop filtering keyboard (so you can
    // move) but DO swallow LMB/RMB down/held (so you only pan the camera -- no stray attacks, and
    // RMB itself doesn't trigger block). Releasing RMB re-Focuses and returns the cursor.
    struct InputDispatchHook
    {
        // Mouse button idCodes (BSWin32MouseDevice ordering): 0 = left, 1 = right.
        static constexpr std::uint32_t kMouseLeft = 0;
        static constexpr std::uint32_t kMouseRight = 1;

        // Unlink every button event matching `shouldDrop` from the list, before any sink sees it.
        static void Unlink(RE::InputEvent** a_evns, bool (*shouldDrop)(RE::ButtonEvent*))
        {
            if (!a_evns) {
                return;
            }
            RE::InputEvent* prev = nullptr;
            RE::InputEvent* cur = *a_evns;
            while (cur) {
                bool drop = false;
                if (cur->eventType == RE::INPUT_EVENT_TYPE::kButton) {
                    if (auto* be = cur->AsButtonEvent()) {
                        drop = shouldDrop(be);
                    }
                }
                if (drop) {
                    if (prev) {
                        prev->next = cur->next;
                    } else {
                        *a_evns = cur->next;
                    }
                    cur = cur->next;  // node unlinked; prev stays put
                } else {
                    prev = cur;
                    cur = cur->next;
                }
            }
        }

        // Tracks which scancodes we "own" -- keys whose DOWN happened while we were capturing.
        // A key already held when capture begins is the game's (e.g. the Left Shift held to open
        // the panel, which is also Skyrim's walk modifier); we must pass ALL its events or its
        // run/walk state gets stuck. Only fresh presses under capture are fully swallowed.
        static inline bool g_owned[256] = {};
        static inline bool g_wasFiltering = false;

        // Normal mode keyboard suppression (stateful -- see g_owned). Escape is always passed so
        // EscInputSink can close the panel.
        static void FilterKeyboard(RE::InputEvent** a_evns)
        {
            if (!a_evns) {
                return;
            }
            RE::InputEvent* prev = nullptr;
            RE::InputEvent* cur = *a_evns;
            while (cur) {
                bool drop = false;
                if (cur->eventType == RE::INPUT_EVENT_TYPE::kButton) {
                    if (auto* be = cur->AsButtonEvent();
                        be && be->GetDevice() == RE::INPUT_DEVICE::kKeyboard) {
                        const std::uint32_t sc = be->GetIDCode();
                        if (sc < 256 && sc != 0x01 /* DIK_ESCAPE */) {
                            if (be->IsDown()) {
                                g_owned[sc] = true;   // fresh press under capture -> ours
                                drop = true;
                            } else if (be->IsHeld()) {
                                drop = g_owned[sc];   // ours: swallow; pre-held (game's): pass
                            } else if (be->IsUp()) {
                                if (g_owned[sc]) {     // our press ends -> swallow + release ownership
                                    drop = true;
                                    g_owned[sc] = false;
                                }
                                // pre-held key releasing -> PASS so the game completes its press
                            }
                        }
                    }
                }
                if (drop) {
                    if (prev) {
                        prev->next = cur->next;
                    } else {
                        *a_evns = cur->next;
                    }
                    cur = cur->next;
                } else {
                    prev = cur;
                    cur = cur->next;
                }
            }
        }
        // Look mode: swallow LMB/RMB down/held so repositioning doesn't attack/block/cast.
        static bool DropMouseClick(RE::ButtonEvent* be)
        {
            return be->GetDevice() == RE::INPUT_DEVICE::kMouse &&
                   (be->GetIDCode() == kMouseLeft || be->GetIDCode() == kMouseRight) && !be->IsUp();
        }
        // Escape, while the panel owns focus: dropped so closing the panel can't ALSO open the
        // game's pause/journal menu underneath -- the two racing left input dead and the game paused.
        static bool DropEscape(RE::ButtonEvent* be)
        {
            return be->GetDevice() == RE::INPUT_DEVICE::kKeyboard && be->GetIDCode() == 0x01 /* DIK_ESCAPE */;
        }

        // The panel's keyboard keys (DirectInput scancodes). Returns the JS name the view's
        // pwKey() expects, or nullptr for everything else. Digits 1-4 arm an action / 0 swaps
        // Transform; they're swallowed too so they don't trigger gameplay favourites/hotkeys.
        static const char* NavKeyName(std::uint32_t sc)
        {
            switch (sc) {
                case 0xC8: return "ArrowUp";
                case 0xD0: return "ArrowDown";
                case 0xCB: return "ArrowLeft";
                case 0xCD: return "ArrowRight";
                case 0x0F: return "Tab";
                case 0xD3: return "Delete";
                case 0x1C: case 0x9C: return "Enter";   // Return + numpad Enter
                case 0x02: return "1";
                case 0x03: return "2";
                case 0x04: return "3";
                case 0x05: return "4";
                case 0x0B: return "0";
                case 0x0C: return "-";   // '-' key -> interrupt dialogue
                case 0x33: return ",";   // ',' key -> toggle continuous mode
                case 0x0D: return "=";   // '=' key -> pause/unpause
                case 0x34: return ".";   // '.' key -> toggle whisper mode
                case 0x35: return "/";   // ? / key -> open controls window
                default:   return nullptr;
            }
        }

        // Swallow the navigation keys before ANY consumer sees them -- PrismaUI feeds CEF from
        // the same InputEvent list other mods' RegisterForKey hooks read, so a key that reaches
        // the page also reaches gameplay (Delete opening Modex, etc.) and the page can't stop it.
        // We drop all phases of these keys and, on the initial press, forward the name to the
        // view, which performs the actual navigation. Only runs while the panel owns input and
        // the text box is NOT focused (typing keeps these keys for editing).
        static void HandleNavKeys(RE::InputEvent** a_evns)
        {
            if (!a_evns) {
                return;
            }
            RE::InputEvent* prev = nullptr;
            RE::InputEvent* cur = *a_evns;
            while (cur) {
                bool drop = false;
                if (cur->eventType == RE::INPUT_EVENT_TYPE::kButton) {
                    if (auto* be = cur->AsButtonEvent();
                        be && be->GetDevice() == RE::INPUT_DEVICE::kKeyboard) {
                        if (const char* nk = NavKeyName(be->GetIDCode())) {
                            if (be->IsDown()) {
                                std::string name = nk;
                                // Keyboard can't otherwise tell the view about modifiers: forward a
                                // distinct "CtrlEnter" so the panel can map Enter = set As / Ctrl+Enter
                                // = set To. (Ctrl, not Shift, to match the mouse gesture -- Shift+click
                                // extends the browser text selection. GetAsyncKeyState reads live key
                                // state regardless of whether we swallow the modifier from game input.)
                                if (name == "Enter" && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
                                    name = "CtrlEnter";
                                }
                                SKSE::GetTaskInterface()->AddTask([name]() {
                                    if (g_prisma && g_viewReady.load() && g_prisma->IsValid(g_view)) {
                                        g_prisma->InteropCall(g_view, "pwKey", name.c_str());
                                    }
                                });
                            }
                            drop = true;  // swallow down/held/up so the key never reaches gameplay
                        }
                    }
                }
                if (drop) {
                    if (prev) {
                        prev->next = cur->next;
                    } else {
                        *a_evns = cur->next;
                    }
                    cur = cur->next;
                } else {
                    prev = cur;
                    cur = cur->next;
                }
            }
        }

        static void EnterLook()
        {
            g_lookMode.store(true);  // flip immediately so this frame stops filtering keyboard
            logger::info("DIAG lookMode ENTER (Unfocus)");
            SKSE::GetTaskInterface()->AddTask([]() {
                if (g_prisma && g_viewReady.load() && g_prisma->IsValid(g_view) &&
                    g_prisma->HasFocus(g_view)) {
                    GuardedUnfocus();  // drop focus menu -> game regains mouselook + movement
                }
            });
        }
        static void ExitLook()
        {
            g_lookMode.store(false);
            logger::info("DIAG lookMode EXIT (Focus)");
            SKSE::GetTaskInterface()->AddTask([]() {
                if (g_prisma && g_viewReady.load() && g_prisma->IsValid(g_view) &&
                    g_panelOpen.load() && !g_prisma->HasFocus(g_view)) {
                    g_appliedPause = EffectivePause();
                    GuardedFocus(g_appliedPause);  // restore cursor + panel interaction
                }
            });
        }

        // A vanilla dialogue (NPC conversation) is open. SkyrimNet drives the stock dialogue system,
        // and Tab is its exit binding -- but while the panel owns input we swallow Tab (-> our action
        // menu), so the player gets stuck unable to leave a conversation they opened Playwright during.
        // While this menu is up we yield the keyboard entirely (nav + capture below), so Tab/arrows/Enter
        // reach the dialogue; capture resumes the moment it closes.
        static bool VanillaDialogueOpen()
        {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
        }

        static void Dispatch(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher, RE::InputEvent** a_evns)
        {
            // Runs on the main thread (game loop). Reads atomics only -- PrismaUI calls are deferred.
            const bool dlgOpen = VanillaDialogueOpen();  // yield the keyboard while an NPC conversation is up
            // 1) RMB transitions toggle look mode (only while the panel is open and not typing).
            if (a_evns) {
                for (auto* e = *a_evns; e; e = e->next) {
                    if (e->eventType != RE::INPUT_EVENT_TYPE::kButton) {
                        continue;
                    }
                    auto* be = e->AsButtonEvent();
                    if (!be || be->GetDevice() != RE::INPUT_DEVICE::kMouse ||
                        be->GetIDCode() != kMouseRight) {
                        continue;
                    }
                    if (be->IsDown() && g_panelOpen.load() && !g_typing.load() && !g_lookMode.load()) {
                        EnterLook();
                    } else if (be->IsUp() && g_lookMode.load()) {
                        ExitLook();
                    }
                }
            }
            // 1b) Escape while the panel owns focus: exit the text box (if typing) else close the
            //     panel, and DROP the key here so it can't ALSO reach MenuControls and open the
            //     pause/journal menu underneath. A BSTEventSink can't block input, so the old
            //     EscInputSink collapsed the panel but the same Escape still opened the journal --
            //     the two raced and left input dead with the game stuck paused. Unlinking it in the
            //     dispatch hook (before any sink) is the only way to actually suppress it. Gated on
            //     HasFocus (what EscInputSink used), so it supersedes that sink cleanly.
            if (a_evns) {
                bool escDown = false;
                for (auto* e = *a_evns; e; e = e->next) {
                    if (e->eventType != RE::INPUT_EVENT_TYPE::kButton) {
                        continue;
                    }
                    auto* be = e->AsButtonEvent();
                    if (be && be->GetDevice() == RE::INPUT_DEVICE::kKeyboard &&
                        be->GetIDCode() == 0x01 /* DIK_ESCAPE */ && be->IsDown()) {
                        escDown = true;
                        break;
                    }
                }
                if (escDown && g_prisma && g_viewReady.load() && g_prisma->IsValid(g_view) &&
                    g_prisma->HasFocus(g_view)) {
                    if (g_typing.load()) {
                        SKSE::GetTaskInterface()->AddTask([]() {
                            if (g_prisma && g_viewReady.load() && g_prisma->IsValid(g_view)) {
                                g_prisma->InteropCall(g_view, "pwBlurText", "");
                            }
                        });
                    } else {
                        CollapseFromCode();
                    }
                    Unlink(a_evns, &DropEscape);
                }
            }
            // 2) Panel navigation keys: swallow + forward to the view (always, regardless of
            //    CaptureKeys -- nav keys must never leak to other mods' hotkeys). Skipped while
            //    typing (the box needs them for editing) or in look mode.
            if (g_panelOpen.load() && !g_typing.load() && !g_lookMode.load() && !dlgOpen &&
                g_prisma && g_viewReady.load()) {
                HandleNavKeys(a_evns);
            }
            // 3) Filter for the current mode. While TYPING we ALWAYS swallow keyboard input from
            //    other consumers: the text box receives keys via CEF independently, so removing them
            //    from the game's input list here doesn't affect editing -- it only stops them leaking
            //    to gameplay/other mods (Tab opening OAR's menu, Delete reaching Modex, etc.). While
            //    NOT typing, swallow the non-nav keys only when CaptureKeys is on (the nav keys were
            //    already taken by HandleNavKeys above).
            const bool kbFilter = g_panelOpen.load() && !g_lookMode.load() && !dlgOpen &&
                                  (g_typing.load() || g_captureKeys);
            if (g_wasFiltering && !kbFilter) {
                for (auto& b : g_owned) {  // leaving keyboard-capture: forget owned presses
                    b = false;
                }
            }
            g_wasFiltering = kbFilter;

            if (g_lookMode.load()) {
                if (g_panelOpen.load()) {
                    Unlink(a_evns, &DropMouseClick);  // swallow clicks; keyboard + mouselook pass through
                }
            } else if (kbFilter) {
                FilterKeyboard(a_evns);
            }
            _orig(a_dispatcher, a_evns);
        }

        static void Install()
        {
            // 0x7B into PollInputDevices = the call that hands the InputEvent list to sinks.
            REL::Relocation<std::uintptr_t> hookSite{ REL::RelocationID(67315, 68617), 0x7B };
            auto& trampoline = SKSE::GetTrampoline();
            _orig = trampoline.write_call<5>(hookSite.address(), &Dispatch);
            logger::info("Input-dispatch hook installed @ {:X}", hookSite.address());
        }

    private:
        static inline REL::Relocation<decltype(&Dispatch)> _orig;
    };

    // interrupt / whisper / continuous are SkyrimNet "simulate the hotkey" natives that the engine
    // ignores while a menu owns input -- and our panel holds menu focus the whole time it's open
    // (SkyrimNet's own wheel sidesteps this by closing before it fires). So briefly release focus --
    // the panel stays visible, like RMB look-mode -- fire the trigger via Papyrus, then refocus once
    // the VM has had time to run it. The sim checks menu state at execution time, so focus must still
    // be released when it actually runs; 450ms covers the ModEvent->Papyrus latency.
    void FireUnfocused(const std::string& s)
    {
        SKSE::GetTaskInterface()->AddTask([s]() {
            GuardedUnfocus();
            SendCommandToPapyrus(s);
        });
        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(450));
            SKSE::GetTaskInterface()->AddTask([]() {
                if (g_panelOpen.load() && !g_lookMode.load() && g_prisma && g_viewReady.load() &&
                    g_prisma->IsValid(g_view) && !g_prisma->HasFocus(g_view)) {
                    GuardedFocus(g_appliedPause);
                }
            });
        }).detach();
    }

    // Put UTF-8 text on the Windows clipboard as CF_UNICODETEXT. The CEF view's
    // document.execCommand("copy") no-ops in this overlay, so the panel routes its
    // "copy" buttons through here for a real ctrl+c-equivalent.
    void SetClipboardUtf8(const std::string& a_utf8)
    {
        if (!OpenClipboard(nullptr)) {
            return;
        }
        EmptyClipboard();
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, a_utf8.c_str(), static_cast<int>(a_utf8.size()), nullptr, 0);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (static_cast<size_t>(wlen) + 1) * sizeof(wchar_t));
        if (hMem) {
            auto* dst = static_cast<wchar_t*>(GlobalLock(hMem));
            if (dst) {
                if (wlen > 0) {
                    MultiByteToWideChar(CP_UTF8, 0, a_utf8.c_str(), static_cast<int>(a_utf8.size()), dst, wlen);
                }
                dst[wlen] = L'\0';
                GlobalUnlock(hMem);
                SetClipboardData(CF_UNICODETEXT, hMem);  // system takes ownership of hMem on success
            } else {
                GlobalFree(hMem);
            }
        }
        CloseClipboard();
    }

    // ---- JS listener callbacks (free functions: no captures) ----
    void OnJsCommand(const char* a_arg)
    {
        const std::string s = a_arg ? a_arg : "";
        logger::info("JS command -> {}", s.empty() ? "(null)" : s.c_str());
        // These no-op while the panel holds focus (they're SkyrimNet "simulate the hotkey"
        // natives, gated on menu state) -- fire them with focus briefly released. gamemaster /
        // npcreact / continarrate are the same class as whisper/interrupt/continuous.
        const std::string action = s.substr(0, s.find('|'));
        if (action == "whisper" || action == "interrupt" || action == "continuous" ||
            action == "gamemaster" || action == "npcreact" || action == "continarrate") {
            FireUnfocused(s);
            return;
        }
        SendCommandToPapyrus(s);
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
                g_panelOpen.store(false);
                g_lookMode.store(false);
                g_appliedPause = false;
                GuardedUnfocus();
                g_prisma->InteropCall(g_view, "pwSetOpen", "0");  // collapse to dot
            }
        });
    }

    void OnJsRefresh(const char*) { FetchConfigAsync(); }  // re-pull live radius + whisper, then PushData

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
        } else if (key == "allactors") {
            // Power-user escape hatch: union the local engine enumeration into the cast (see BuildNpcJson).
            g_allActors.store(on);
            WritePrivateProfileStringA("Behavior", "AllActors", on ? "1" : "0", INI_PATH);
            logger::info("Config allActors -> {}", on);
            PushData();  // rebuild the cast immediately so the toggle feels instant (don't wait for the 1 Hz poll)
        } else if (key == "typing") {
            // Text box gained/lost focus -- transient auto-pause so keystrokes
            // don't leak to other mods' hotkeys while composing a line.
            // Ignore the focus/blur echo from our own Unfocus->Focus cycle, or the
            // pause state would thrash (blur->unpause->focus->pause->...).
            // Swallow only the echoes our own Unfocus->Focus cycle produces -- counted exactly,
            // not a blanket time window. The old window ate genuine focus changes that landed
            // within 500ms (e.g. mashing Enter to re-open the log editor), wedging the typing
            // state. The <500ms cap here is only a safety net: if an expected echo never arrives,
            // the counter self-clears instead of swallowing real events forever.
            if (g_pendingTypingEchoes.load() > 0 && (GetTickCount64() - g_reapplyTick < 500)) {
                g_pendingTypingEchoes.fetch_sub(1);
                return;
            }
            g_pendingTypingEchoes.store(0);
            if (g_typing == on) {
                return;
            }
            g_typing = on;
            ReapplyPause();
        } else if (key == "typingEnd") {
            // Definitive end-of-typing from the view: the editable field was removed
            // (log-edit committed/cancelled), not merely blurred. Unlike "typing", this
            // is never an echo of our own Unfocus->Focus cycle (the field is gone, so the
            // re-Focus can't land on it and can't thrash), so it BYPASSES the 500ms echo
            // guard. Without this, a quick edit + Enter -- committing within 500ms of the
            // focus-driven reapply -- has its typing|0 swallowed, leaving g_typing stuck
            // true: the game stays paused and the input hook stops forwarding nav keys.
            g_pendingTypingEchoes.store(0);
            if (g_typing) {
                g_typing = false;
                ReapplyPause();
            }
        }
    }

    // ---- editable log JS listeners (JS -> DLL; HTTP happens off-thread) ----
    void OnJsLogFetch(const char*) { FetchLogAsync(); }

    void OnJsActionsFetch(const char*) { FetchActionsAsync(); }

    void OnJsActionsEligible(const char* a_arg)
    {
        // arg = "<castToken>|<uuid>" (uuid may be empty for the fallback enumeration / player)
        const std::string s = a_arg ? a_arg : "";
        const auto bar = s.find('|');
        if (bar == std::string::npos) {
            FetchEligibleAsync(s, "");
        } else {
            FetchEligibleAsync(s.substr(0, bar), s.substr(bar + 1));
        }
    }

    void OnJsTemplateFetch(const char* a_arg) { if (a_arg && *a_arg) FetchTemplateAsync(a_arg); }

    // Scenario launcher: read the scenarios/ folder off disk and push the array to the view.
    void OnJsScenariosFetch(const char*) { PushInterop("pwSetScenarios", ReadScenarioFiles()); }

    // Editor save: payload = "<stem>\t<json>". Write the file, then re-push the refreshed list so
    // the launcher (and the file-identity envelope) reflect the change immediately.
    void OnJsScenarioSave(const char* a_arg)
    {
        const std::string s = a_arg ? a_arg : "";
        const auto tab = s.find('\t');
        if (tab == std::string::npos) {
            return;
        }
        WriteScenarioFile(s.substr(0, tab), s.substr(tab + 1));
        PushInterop("pwSetScenarios", ReadScenarioFiles());
    }

    void OnJsScenarioDelete(const char* a_arg)
    {
        if (a_arg && *a_arg) {
            DeleteScenarioFile(a_arg);
        }
        PushInterop("pwSetScenarios", ReadScenarioFiles());
    }

    void OnJsLogDelete(const char* a_arg)
    {
        if (a_arg && *a_arg) {
            DeleteEventAsync(a_arg);
        }
    }

    // Copy arbitrary text to the OS clipboard on demand (builder prompt, etc.).
    void OnJsClipboard(const char* a_arg)
    {
        if (a_arg && *a_arg) {
            SetClipboardUtf8(a_arg);
        }
    }

    void OnJsLogEdit(const char* a_arg)
    {
        if (!a_arg) {
            return;
        }
        std::string s = a_arg;  // "id|type|new text" (text may itself contain '|')
        const auto b1 = s.find('|');
        if (b1 == std::string::npos) {
            return;
        }
        const auto b2 = s.find('|', b1 + 1);
        if (b2 == std::string::npos) {
            return;
        }
        EditEventAsync(s.substr(0, b1), s.substr(b1 + 1, b2 - b1 - 1), s.substr(b2 + 1));
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
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuWatch::GetSingleton());  // clear in-game at the main menu
        }
        // bAlwaysRunByDefault lives in the PREFS collection (SkyrimPrefs.ini), not GetINISetting --
        // reading it from the wrong place was why the run/walk guard never fired.
        if (auto* prefs = RE::INIPrefSettingCollection::GetSingleton()) {
            g_alwaysRun = prefs->GetSetting("bAlwaysRunByDefault:Controls");
        }
        if (!g_alwaysRun) {
            g_alwaysRun = RE::GetINISetting("bAlwaysRunByDefault:Controls");
        }
        logger::info("alwaysRun setting: {}", g_alwaysRun ? "found" : "NULL");
        ReadWebServerConfig();
        g_pauseGame = ReadPauseGame();
        g_captureKeys = GetPrivateProfileIntA("Behavior", "CaptureKeys", 0, INI_PATH) != 0;
        logger::info("CaptureKeys={}", g_captureKeys);
        g_allActors.store(GetPrivateProfileIntA("Behavior", "AllActors", 0, INI_PATH) != 0);
        g_maxCast = GetPrivateProfileIntA("Behavior", "MaxCast", 50, INI_PATH);
        if (g_maxCast < 1) {
            g_maxCast = 1;
        }
        logger::info("AllActors={} MaxCast={}", g_allActors.load(), g_maxCast);

        g_view = g_prisma->CreateView("Playwright/index.html", [](PrismaView a_view) {
            logger::info("Playwright view DOM ready ({})", a_view);
            // Register listeners HERE (DOM is ready / globals exist) with FLAT
            // names -- PrismaUI creates window.<name> unconditionally. Doing this
            // before DOM-ready, or with dotted names, silently fails to bind.
            g_prisma->RegisterJSListener(a_view, "pwCommand", OnJsCommand);
            g_prisma->RegisterJSListener(a_view, "pwClose", OnJsClose);
            g_prisma->RegisterJSListener(a_view, "pwRefresh", OnJsRefresh);
            g_prisma->RegisterJSListener(a_view, "pwConfig", OnJsConfig);
            g_prisma->RegisterJSListener(a_view, "pwLogFetch", OnJsLogFetch);
            g_prisma->RegisterJSListener(a_view, "pwLogEdit", OnJsLogEdit);
            g_prisma->RegisterJSListener(a_view, "pwLogDelete", OnJsLogDelete);
            g_prisma->RegisterJSListener(a_view, "pwActionsFetch", OnJsActionsFetch);
            g_prisma->RegisterJSListener(a_view, "pwActionsEligible", OnJsActionsEligible);
            g_prisma->RegisterJSListener(a_view, "pwTemplateFetch", OnJsTemplateFetch);
            g_prisma->RegisterJSListener(a_view, "pwScenariosFetch", OnJsScenariosFetch);
            g_prisma->RegisterJSListener(a_view, "pwScenarioSave", OnJsScenarioSave);
            g_prisma->RegisterJSListener(a_view, "pwScenarioDelete", OnJsScenarioDelete);
            g_prisma->RegisterJSListener(a_view, "pwClipboard", OnJsClipboard);
            // The view stays SHOWN at all times so its corner dot widget is always
            // visible; "closed" just collapses to the dot (unfocused, click-through
            // via pointer-events:none on empty areas). It is never Hidden.
            g_viewReady.store(true);
            g_prisma->InteropCall(a_view, "pwSetOpen", "0");
            g_prisma->InteropCall(a_view, "pwSetDirector", IsDirectorOn() ? "1" : "0");
            // Push the data-driven per-NPC action menu (button fragments). Empty -> the view
            // keeps its built-in default menu, so this never breaks an older/partial install.
            // Also auto-discover the faction roster the fragments reference (for npc.fx /
            // showWhen "faction:..."), from the same fragment text.
            {
                const std::string menu = ReadMenuFragments();
                BuildMenuFactions(menu);
                if (!menu.empty()) {
                    g_prisma->InteropCall(a_view, "pwSetMenu", menu.c_str());
                }
            }
            FetchConfigAsync();  // prime interaction radius + whisper state from SkyrimNet
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

        // Hook the input dispatch so the panel can steal keys from other mods/vanilla while
        // open-and-not-typing (a plain sink can't -- see InputDispatchHook).
        InputDispatchHook::Install();

        // 1 Hz cast-list refresh while open + unpaused (keeps the list current as you reposition).
        StartActorPoll();

        logger::info("Playwright PrismaUI bridge initialized.");
    }

    void SKSEMessageHandler(SKSE::MessagingInterface::Message* a_message)
    {
        switch (a_message->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            OnDataLoaded();
            break;
        case SKSE::MessagingInterface::kPostLoadGame:  // save loaded (Continue / Load)
        case SKSE::MessagingInterface::kNewGame:       // new game started
            g_inGame.store(true);
            PushInterop("pwSetIngame", "1");           // reveal the persistent log now we're in the world
            break;
        default:
            break;
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
