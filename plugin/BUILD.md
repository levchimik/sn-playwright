# Building SNPlaywright.dll

A thin CommonLibSSE-NG SKSE plugin: it owns the Playwright PrismaUI view,
enumerates the nearby cast, and forwards button clicks to `PW_Controller.psc` via
the `PW_PrismaCommand` SKSE ModEvent. All game logic stays in Papyrus.

## Requirements
- [xmake](https://xmake.io) 2.8.2+  (`winget install Xmake-io.Xmake`)
- A C++23 MSVC toolset (Visual Studio 2022/2026, "Desktop development with C++")

## One-time: fetch CommonLibSSE-NG
`lib/` is gitignored (it's large). Clone the runtime-agnostic NG fork into it:

```bat
git clone --depth 1 --branch ng --recurse-submodules --shallow-submodules ^
  https://github.com/alandtse/CommonLibVR.git lib/commonlibsse-ng
```

## Build
```bat
xmake f -m release -y
xmake build
```
Output: `build/windows/x64/release/SNPlaywright.dll`.

## Deploy
Copy into the Playwright mod:
- `SNPlaywright.dll` + `SNPlaywright.ini` → `SKSE/Plugins/`
- `view/index.html` → `PrismaUI/views/Playwright/index.html`

## Bridge contract
- **JS → C++** (PrismaUI `RegisterJSListener`): `playwright.command("action|targetId|text")`,
  `playwright.close`, `playwright.ready`, `playwright.refresh`.
- **C++ → JS** (`InteropCall`): `pwSetData(json)` where json =
  `{"directorMode":bool,"npcs":[{"name","id","dist","state","player"?}]}`.
- **C++ → Papyrus** (`SendEvent` ModCallbackEvent): `PW_PrismaCommand`, strArg =
  `"action|targetId|text"`. `targetId` is `"player"`, a `0x`-runtime-FormID, or empty.
- Panel open/close is Papyrus-driven: the MCM-bound "Open PrismaUI Panel" key (default
  Shift+F11) makes `PW_Controller` fire the `PW_PrismaToggle` ModEvent, which the DLL's
  `PrismaModEventSink` catches and turns into `ToggleMenu()`. The DLL does NOT read a
  toggle key from the ini (the ini holds only `[Behavior] PauseGame`).
