Scriptname PW_Controller extends ReferenceAlias
{Playwright controller (v0.4). Hosts the hotkeys + radial wheel.
 Every key is MCM-rebindable (see PW_MCM) and gated behind a modifier key,
 SeverActions-style: while RequireModifier is on, no SND key fires unless the
 modifier (default Left Shift) is held. Action functions are public so the MCM
 and the wheel can invoke them directly.}

; --- Rebindable keys (DXScanCodes). Unbound (-1) by default — set them in the MCM. ---
Int Property WheelKey      = -1 Auto   ; open the radial wheel
Int Property ToggleKey     = -1 Auto   ; toggle Director Mode
Int Property NarrateKey    = -1 Auto   ; narrate a scene event (text box); a nearby NPC voices it
Int Property PromptKey     = -1 Auto   ; prompt a character to speak (no text)
Int Property SayKey        = -1 Auto   ; force the crosshair NPC to say literal text (no LLM)
Int Property ThinkKey      = -1 Auto   ; force the crosshair NPC to think a literal thought (no LLM)
Int Property TransformKey  = -1 Auto   ; write a line as yourself; the LLM rephrases it and you speak it
Int Property AsleepKey       = -1 Auto ; toggle Deep Sleep (unconscious) on crosshair NPC
Int Property AsleepSelfKey   = -1 Auto ; toggle Deep Sleep on yourself
Int Property SleeptalkKey    = -1 Auto ; toggle Sleep-talk (deaf, but murmurs) on crosshair NPC
Int Property SleeptalkSelfKey = -1 Auto ; toggle Sleep-talk on yourself

; --- Modifier arming gate. ---
Int  Property ModifierKey     = 42 Auto    ; DXScanCode held to arm SND keys (42 = LShift; -1/0 = none)
Bool Property RequireModifier = true Auto  ; if true, SND keys do nothing unless ModifierKey is held

; --- Options. ---
Bool Property SendToBed = false Auto ; if true, EITHER sleep mode walks the NPC to the nearest bed (via SeverActions) to sleep there

; --- Sleep-talk murmuring (ambient dream-speech channel; does NOT route through the
;     speaker selector, so no one is ever pulled into a conversation with the sleeper). ---
Bool  Property SleeptalkMurmur = true Auto  ; if true, sleep-talkers occasionally mutter dream fragments aloud
Float Property MurmurInterval  = 30.0 Auto  ; seconds between murmur checks
Float Property MurmurChance    = 0.35 Auto  ; per-check chance each nearby sleep-talker murmurs

Bool _textBusy = false     ; guards the shared text-entry menu against re-entry
Bool _murmurLoop = false   ; true while the murmur OnUpdate loop is scheduled
Bool _pdWasEnabled = true  ; remembers the player's autonomous-thought setting to restore on OFF

; ------------------------------------------------------------------ factions
Faction Function GetBlacklistFaction()
    Return Game.GetFormFromFile(0x12DB, "SkyrimNet.esp") as Faction
EndFunction

Faction Function GetAsleepFaction()
    Return Game.GetFormFromFile(0x801, "SNPlaywright.esp") as Faction
EndFunction

Faction Function GetSleeptalkFaction()
    Return Game.GetFormFromFile(0x803, "SNPlaywright.esp") as Faction
EndFunction

; Format a FormID as a "0x"-prefixed 8-digit hex string. We hand the *placed* bed
; reference's FormID to SeverActions; building the hex ourselves sidesteps the
; signed-Int decimal ambiguity for FormIDs with a load-order index > 0x7F (e.g.
; beds in mod-added inns/homes), which "id as String" would render as a negative
; decimal that the native resolver may not parse as unsigned.
String Function FormIDToHex(Int id)
    String digits = "0123456789ABCDEF"
    String out = ""
    Int i = 0
    While i < 8
        out = StringUtil.GetNthChar(digits, Math.LogicalAnd(id, 15)) + out
        id = Math.RightShift(id, 4)
        i += 1
    EndWhile
    Return "0x" + out
EndFunction

; Route a sleeping NPC to the nearest bed using SeverActions' walk-to-furniture
; system. Never the player. Needs SeverActions loaded. SeverActions' native
; FindFurnitureByFormID resolves a base FormID to the nearest placed instance and
; its sandbox package walks the actor there to use it (i.e. sleep).
Function SendActorToBed(Actor t)
    If t == Game.GetPlayer()
        Return
    EndIf
    If Game.GetModByName("SeverActions.esp") == 255
        Debug.Notification("Send to bed needs SeverActions installed")
        Return
    EndIf
    ObjectReference bed = FindNearestBed(t)
    If !bed
        Debug.Notification("No bed nearby for " + t.GetDisplayName())
        Return
    EndIf
    ; Pass the PLACED reference's FormID (as hex) so SeverActions resolves THIS exact
    ; bed directly. Passing the base object's FormID instead forces a BaseID->RefID
    ; fallback limited to 500u of the actor, which silently fails for any bed farther
    ; than that and leaves the NPC standing while our notification still fires.
    SeverActions_Furniture.UseFurniture_Global_Execute(t, FormIDToHex(bed.GetFormID()))
    Debug.Notification(t.GetDisplayName() + " is heading to bed")
EndFunction

; Get a sent-to-bed NPC back up (no-op if they aren't using furniture).
Function StopActorBed(Actor t)
    If t == Game.GetPlayer() || Game.GetModByName("SeverActions.esp") == 255
        Return
    EndIf
    SeverActions_Furniture.StopUsingFurniture_Global_Execute(t)
EndFunction

; Nearest usable bed in the actor's cell. Beds are detected by base-object name
; containing "Bed" (covers "Bed", "Double Bed", "Bed Roll", etc.) — there is no
; single universal bed keyword. Returns None if none found.
ObjectReference Function FindNearestBed(Actor a)
    Cell c = a.GetParentCell()
    If !c
        Return None
    EndIf
    ObjectReference best = None
    Float bestDist = 4096.0 ; doubles as the max search distance (~58m) — never send them on a cross-cell trek
    Int n = c.GetNumRefs(40) ; 40 = kFurniture
    Int i = 0
    While i < n
        ObjectReference r = c.GetNthRef(i, 40)
        If r && r.Is3DLoaded() && !r.IsDisabled() && !r.IsFurnitureInUse()
            Form base = r.GetBaseObject()
            If base && StringUtil.Find(base.GetName(), "Bed") >= 0
                Float d = a.GetDistance(r)
                If d < bestDist
                    bestDist = d
                    best = r
                EndIf
            EndIf
        EndIf
        i += 1
    EndWhile
    Return best
EndFunction

; ------------------------------------------------------------------ lifecycle
Event OnInit()
    RegisterKeys()
    RegisterDecorators()
EndEvent

Event OnPlayerLoadGame()
    RegisterKeys()
    RegisterDecorators()
EndEvent

; SkyrimNet decorators are runtime registrations that don't survive a reload, so
; (re)register them every load — same pattern SeverActions uses. These expose the
; "deaf window" (game-seconds) so event_history.prompt can permanently drop events
; an actor couldn't perceive while asleep.
Function RegisterDecorators()
    SkyrimNetApi.RegisterDecorator("pw_deaf_start", "PW_Controller", "PW_DeafStart")
    SkyrimNetApi.RegisterDecorator("pw_deaf_end", "PW_Controller", "PW_DeafEnd")
EndFunction

; --- Deaf-window tracking (so a woken actor doesn't "remember" what was said while
;     they were asleep). Stored per-actor in game-seconds (GetCurrentGameTime*86400,
;     the same unit as SkyrimNet's event.gameTime). DeafEnd == 0 means "still deaf".
Function BeginDeaf(Actor t)
    StorageUtil.SetFloatValue(t, "PW_DeafStart", Utility.GetCurrentGameTime() * 86400.0)
    StorageUtil.SetFloatValue(t, "PW_DeafEnd", 0.0)
EndFunction

Function EndDeaf(Actor t)
    StorageUtil.SetFloatValue(t, "PW_DeafEnd", Utility.GetCurrentGameTime() * 86400.0)
EndFunction

; Decorator bodies (called by SkyrimNet as pw_deaf_start(uuid) / pw_deaf_end(uuid)).
; Returned as strings; the prompt coerces with float().
String Function PW_DeafStart(Actor a) Global
    Return StorageUtil.GetFloatValue(a, "PW_DeafStart", 0.0) as String
EndFunction

String Function PW_DeafEnd(Actor a) Global
    Return StorageUtil.GetFloatValue(a, "PW_DeafEnd", 0.0) as String
EndFunction

Function RegisterKeys()
    UnregisterForAllKeys()
    If WheelKey > 0
        RegisterForKey(WheelKey)
    EndIf
    If ToggleKey > 0
        RegisterForKey(ToggleKey)
    EndIf
    If NarrateKey > 0
        RegisterForKey(NarrateKey)
    EndIf
    If PromptKey > 0
        RegisterForKey(PromptKey)
    EndIf
    If SayKey > 0
        RegisterForKey(SayKey)
    EndIf
    If ThinkKey > 0
        RegisterForKey(ThinkKey)
    EndIf
    If TransformKey > 0
        RegisterForKey(TransformKey)
    EndIf
    If AsleepKey > 0
        RegisterForKey(AsleepKey)
    EndIf
    If AsleepSelfKey > 0
        RegisterForKey(AsleepSelfKey)
    EndIf
    If SleeptalkKey > 0
        RegisterForKey(SleeptalkKey)
    EndIf
    If SleeptalkSelfKey > 0
        RegisterForKey(SleeptalkSelfKey)
    EndIf
EndFunction

Event OnKeyDown(Int keyCode)
    If Utility.IsInMenuMode()
        Return
    EndIf
    ; Modifier arming gate (SeverActions pattern): swallow the key unless the
    ; configured modifier is currently held.
    If RequireModifier && ModifierKey > 0 && !Input.IsKeyPressed(ModifierKey)
        Return
    EndIf
    If keyCode == WheelKey
        OpenWheel()
    ElseIf keyCode == ToggleKey
        ToggleDirector()
    ElseIf keyCode == NarrateKey
        Narrate()
    ElseIf keyCode == PromptKey
        PromptSpeak()
    ElseIf keyCode == SayKey
        Say()
    ElseIf keyCode == ThinkKey
        Think()
    ElseIf keyCode == TransformKey
        Transform()
    ElseIf keyCode == AsleepKey
        AsleepCrosshair()
    ElseIf keyCode == AsleepSelfKey
        AsleepSelf()
    ElseIf keyCode == SleeptalkKey
        SleeptalkCrosshair()
    ElseIf keyCode == SleeptalkSelfKey
        SleeptalkSelf()
    EndIf
EndEvent

; ------------------------------------------------------------------ status
Bool Function IsDirectorOn()
    Faction blf = GetBlacklistFaction()
    Return (blf != None) && Game.GetPlayer().IsInFaction(blf)
EndFunction

; ------------------------------------------------------------------ director
Function ToggleDirector()
    Faction blf = GetBlacklistFaction()
    If !blf
        Debug.Notification("Director Mode: SkyrimNet faction not found")
        Return
    EndIf
    Actor pl = Game.GetPlayer()
    If pl.IsInFaction(blf)
        ; --- Director Mode OFF: rejoin the scene ---
        pl.RemoveFromFaction(blf)
        If _pdWasEnabled
            SkyrimNetApi.PatchConfig("PlayerDialogue", "{\"enabled\": true}")
        EndIf
        Debug.Notification("Director Mode: OFF")
    Else
        ; --- Director Mode ON: leave the scene ---
        pl.AddToFaction(blf)
        _pdWasEnabled = SkyrimNetApi.GetConfigBool("PlayerDialogue", "enabled", true)
        SkyrimNetApi.PatchConfig("PlayerDialogue", "{\"enabled\": false}")
        Debug.Notification("Director Mode: ON - you have left the scene")
    EndIf
EndFunction

; ------------------------------------------------------------------ deep sleep (unconscious)
Function AsleepCrosshair()
    Actor t = Game.GetCurrentCrosshairRef() as Actor
    If !t
        Debug.Notification("Deep Sleep: look at an actor first")
        Return
    EndIf
    ToggleAsleepActor(t)
EndFunction

Function AsleepSelf()
    ToggleAsleepActor(Game.GetPlayer())
EndFunction

; Deep sleep: deaf AND unresponsive — others see them as unconscious, and they are
; hard-excluded from the speaker selector (cannot be picked to talk).
Function ToggleAsleepActor(Actor t)
    Faction slf = GetAsleepFaction()
    Faction stf = GetSleeptalkFaction()
    If !slf
        Debug.Notification("Deep Sleep faction not found")
        Return
    EndIf
    If t.IsInFaction(slf)
        t.RemoveFromFaction(slf)
        EndDeaf(t)
        StopActorBed(t)
        SkyrimNetApi.RegisterPersistentEvent(t.GetDisplayName() + " stirs and wakes up.", t, None)
        Debug.Notification(t.GetDisplayName() + " is awake")
    Else
        ; the two sleep modes are mutually exclusive
        Bool wasTalk = stf && t.IsInFaction(stf)
        If wasTalk
            t.RemoveFromFaction(stf)
        EndIf
        t.AddToFaction(slf)
        ; Open the deaf window only when coming from awake — a deep<->talk switch is one
        ; continuous deaf period, so we keep the original start time.
        If !wasTalk
            BeginDeaf(t)
        EndIf
        ; Only (re)position to bed when coming from awake — a deep<->talk switch keeps
        ; whatever furniture state they already had, so we don't bounce them out of bed.
        If SendToBed && !wasTalk
            SendActorToBed(t)
        EndIf
        ; Tell SkyrimNet so nearby NPCs perceive the state change. RegisterPersistentEvent
        ; informs without triggering a dialogue reaction, so it adds no new leak vector.
        SkyrimNetApi.RegisterPersistentEvent(t.GetDisplayName() + " has fallen into a deep, unresponsive sleep.", t, None)
        Debug.Notification(t.GetDisplayName() + " is in a deep sleep")
    EndIf
EndFunction

; ------------------------------------------------------------------ sleep-talk
Function SleeptalkCrosshair()
    Actor t = Game.GetCurrentCrosshairRef() as Actor
    If !t
        Debug.Notification("Sleep-talk: look at an actor first")
        Return
    EndIf
    ToggleSleeptalkActor(t)
EndFunction

Function SleeptalkSelf()
    ToggleSleeptalkActor(Game.GetPlayer())
EndFunction

; Sleep-talk: deaf (perceives nothing) but may still mutter dream-speech via the ambient
; murmur loop (NOT the speaker selector), so others hear murmuring without being routed
; into a conversation with them. Mutually exclusive with deep sleep.
Function ToggleSleeptalkActor(Actor t)
    Faction stf = GetSleeptalkFaction()
    Faction slf = GetAsleepFaction()
    If !stf
        Debug.Notification("Sleep-talk faction not found")
        Return
    EndIf
    If t.IsInFaction(stf)
        t.RemoveFromFaction(stf)
        EndDeaf(t)
        StopActorBed(t)
        SkyrimNetApi.RegisterPersistentEvent(t.GetDisplayName() + " stirs and wakes up.", t, None)
        Debug.Notification(t.GetDisplayName() + " is awake")
    Else
        Bool wasDeep = slf && t.IsInFaction(slf)
        If wasDeep
            t.RemoveFromFaction(slf)
        EndIf
        t.AddToFaction(stf)
        ; Continuous deaf period across a deep<->talk switch — only open the window when
        ; coming from awake.
        If !wasDeep
            BeginDeaf(t)
        EndIf
        ; Same walk-to-bed as Deep Sleep when enabled; skip on a deep<->talk switch so we
        ; keep their existing furniture state instead of bouncing them out of bed.
        If SendToBed && !wasDeep
            SendActorToBed(t)
        EndIf
        StartMurmurLoop()
        SkyrimNetApi.RegisterPersistentEvent(t.GetDisplayName() + " has drifted into a restless sleep, murmuring softly.", t, None)
        Debug.Notification(t.GetDisplayName() + " is now sleep-talking")
    EndIf
EndFunction

; ------------------------------------------------------------------ sleep-talk murmur loop
; Ambient channel: every MurmurInterval seconds, each loaded sleep-talker near the player
; has a MurmurChance to utter a dream fragment via DirectNarration. This DELIBERATELY
; bypasses the speaker selector (where sleep-talkers are hard-excluded to stop leaks), so
; they murmur without anyone being routed to converse WITH them. dialogue_response.prompt's
; PW_SleeptalkFaction clause shapes the empty-content response into incoherent sleep-talk.
Function StartMurmurLoop()
    If SleeptalkMurmur && !_murmurLoop
        _murmurLoop = true
        RegisterForSingleUpdate(MurmurClampedInterval())
    EndIf
EndFunction

Float Function MurmurClampedInterval()
    Float iv = MurmurInterval
    If iv < 5.0
        iv = 5.0
    EndIf
    Return iv
EndFunction

Event OnUpdate()
    If !SleeptalkMurmur
        _murmurLoop = false
        Return
    EndIf
    Faction stf = GetSleeptalkFaction()
    Actor pl = Game.GetPlayer()
    Cell c = pl.GetParentCell()
    Bool any = false
    If stf && c
        Int n = c.GetNumRefs(43) ; 43 = kActorCharacter
        Int i = 0
        While i < n
            Actor a = c.GetNthRef(i, 43) as Actor
            If a && a != pl && a.Is3DLoaded() && a.IsInFaction(stf)
                any = true
                If pl.GetDistance(a) < 2000.0 && Utility.RandomFloat(0.0, 1.0) < MurmurChance
                    SkyrimNetApi.DirectNarration("", a, None)
                EndIf
            EndIf
            i += 1
        EndWhile
    EndIf
    ; Keep ticking while a sleep-talker is loaded near the player; otherwise let the loop
    ; sleep until the next time someone is put into sleep-talk (or murmuring is toggled on).
    If any
        RegisterForSingleUpdate(MurmurClampedInterval())
    Else
        _murmurLoop = false
    EndIf
EndEvent

; ------------------------------------------------------------------ text entry helper
; Open the shared UIExtensions text box and return what was typed ("" on cancel /
; unavailable). Guards against re-entry via _textBusy. OpenMenu blocks until closed.
String Function PromptText()
    String result = ""
    UITextEntryMenu menu = UIExtensions.GetMenu("UITextEntryMenu") as UITextEntryMenu
    If menu
        menu.SetPropertyString("text", "")
        menu.OpenMenu()
        result = menu.GetResultString()
    Else
        Debug.Notification("UIExtensions unavailable")
    EndIf
    Return result
EndFunction

; ------------------------------------------------------------------ narrate
; Type a scene event; a nearby NPC voices it as a general remark to everyone present
; (DirectNarration with an NPC originator + no target). Works in or out of Director
; Mode — in Director Mode the player is absent, otherwise they're in the audience.
Function Narrate()
    If _textBusy
        Return
    EndIf
    Actor pl = Game.GetPlayer()
    Actor speaker = Game.GetCurrentCrosshairRef() as Actor
    Int tries = 0
    While (!speaker || speaker == pl) && tries < 6
        speaker = Game.FindRandomActorFromRef(pl, 1500.0)
        tries += 1
    EndWhile
    If !speaker || speaker == pl
        Debug.Notification("No NPC nearby to perceive the narration - look at one")
        Return
    EndIf
    _textBusy = true
    String narration = PromptText()
    _textBusy = false
    If narration != ""
        SkyrimNetApi.DirectNarration(narration, speaker, None)
        Debug.Notification("Narrated via " + speaker.GetDisplayName())
    EndIf
EndFunction

; ------------------------------------------------------------------ say (literal NPC line, text-only)
; Inject your exact words as the crosshair NPC's line, verbatim, no LLM. NOTE: this is
; recorded into their dialogue history/context (subtitle + memory) but NOT voiced —
; SkyrimNet has no API to TTS an arbitrary literal line; only LLM-generated lines get
; voiced. Use Transform (DirectNarration) when you want it spoken aloud.
Function Say()
    If _textBusy
        Return
    EndIf
    Actor t = Game.GetCurrentCrosshairRef() as Actor
    If !t || t == Game.GetPlayer()
        Debug.Notification("Say: look at an NPC first")
        Return
    EndIf
    _textBusy = true
    String line = PromptText()
    _textBusy = false
    If line != ""
        SkyrimNetApi.RegisterDialogue(t, line)
        Debug.Notification(t.GetDisplayName() + " says it")
    EndIf
EndFunction

; ------------------------------------------------------------------ think (literal NPC thought)
; Inject a literal, persistent, unvoiced thought into the crosshair NPC, verbatim (no
; LLM). The npc_thoughts schema requires both `npc_name` and `thoughts`, so we hand the
; structured data to RegisterEvent as a JSON object in the content arg — a bare string
; only fills `thoughts` and logs "Missing required field 'npc_name'". JSON is escaped
; via SeverActions' native helper so quotes/backslashes in the thought are safe.
Function Think()
    If _textBusy
        Return
    EndIf
    Actor t = Game.GetCurrentCrosshairRef() as Actor
    If !t || t == Game.GetPlayer()
        Debug.Notification("Think: look at an NPC first")
        Return
    EndIf
    _textBusy = true
    String thought = PromptText()
    _textBusy = false
    If thought != ""
        String data = "{\"npc_name\":\"" + SeverActionsNative.EscapeJsonString(t.GetDisplayName()) + "\",\"thoughts\":\"" + SeverActionsNative.EscapeJsonString(thought) + "\"}"
        SkyrimNetApi.RegisterEvent("npc_thoughts", data, t, None)
        Debug.Notification(t.GetDisplayName() + " thinks it")
    EndIf
EndFunction

; ------------------------------------------------------------------ transform (crosshair NPC line via LLM)
; Write the gist of what you want the crosshair NPC to say; the LLM turns it into a
; line in THAT NPC's voice and the NPC speaks it (the LLM counterpart of Say). Fires
; from the NPC, not the player: DirectNarration with the NPC as originator. Needs a
; crosshair NPC.
Function Transform()
    If _textBusy
        Return
    EndIf
    Actor t = Game.GetCurrentCrosshairRef() as Actor
    If !t || t == Game.GetPlayer()
        Debug.Notification("Transform: look at an NPC first")
        Return
    EndIf
    _textBusy = true
    String line = PromptText()
    _textBusy = false
    If line != ""
        SkyrimNetApi.DirectNarration(line, t, None)
        Debug.Notification(t.GetDisplayName() + " (transformed)")
    EndIf
EndFunction

; ------------------------------------------------------------------ prompt (no text)
; Nudge a character to speak without typing anything — the no-text counterpart of
; Narrate. DirectNarration with EMPTY content registers an ephemeral event that
; triggers an immediate response from the originator. Crosshair NPC = prompt THAT
; character to speak; otherwise a random nearby NPC is prompted. We pass NO target,
; which per the API makes the speaker address "everyone nearby" as a general remark:
; the player stays in the audience (not removed from the scene) but is NOT singled
; out / force-addressed. In Director Mode the player is blacklisted, so they're
; simply not part of that nearby audience — the same call works for both modes.
Function PromptSpeak()
    Actor pl = Game.GetPlayer()
    Actor speaker = Game.GetCurrentCrosshairRef() as Actor
    Int tries = 0
    While (!speaker || speaker == pl) && tries < 6
        speaker = Game.FindRandomActorFromRef(pl, 1500.0)
        tries += 1
    EndWhile
    If !speaker || speaker == pl
        Debug.Notification("No NPC nearby to prompt")
        Return
    EndIf
    SkyrimNetApi.DirectNarration("", speaker, None)
    Debug.Notification("Prompted " + speaker.GetDisplayName() + " to speak")
EndFunction

; ------------------------------------------------------------------ wheel
; UIWheelMenu index -> compass position (reverse-engineered):
;   0=NW(upper-left) 1=W(left) 2=SW(lower-left) 3=S(bottom)
;   4=NE(upper-right) 5=E(right) 6=SE(lower-right) 7=N(top)
; Layout: LEFT column top->down = Director(top), Prompt, Say, Think.
;         RIGHT column top->down = Narrate, Transform, Sleep-talk, Deep Sleep(bottom).
; optionLabelText = the spoke label; optionText = the description shown on highlight.
Function OpenWheel()
    Actor pl = Game.GetPlayer()
    Faction blf = GetBlacklistFaction()
    Faction slf = GetAsleepFaction()
    Faction stf = GetSleeptalkFaction()
    Bool directorOn = (blf != None) && pl.IsInFaction(blf)
    Actor cross = Game.GetCurrentCrosshairRef() as Actor
    Bool haveNpc = cross && cross != pl
    String cname = ""
    If haveNpc
        cname = cross.GetDisplayName()
    EndIf

    UIExtensions.InitMenu("UIWheelMenu")

    ; 7 (top) - Director
    If directorOn
        UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionText", 7, "Exit Scene")
    Else
        UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionText", 7, "Enter Scene")
    EndIf
    UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionLabelText", 7, "Director")
    UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu", "optionEnabled", 7, true)

    ; 0 (upper-left) - Prompt (no text)
    If haveNpc
        UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionText", 0, "Prompt " + cname)
    Else
        UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionText", 0, "Prompt to Speak")
    EndIf
    UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionLabelText", 0, "Prompt")
    UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu", "optionEnabled", 0, true)

    ; 1 (left) - Say (literal NPC speech; needs a crosshair NPC)
    If haveNpc
        UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionText", 1, "Say (as " + cname + ")")
        UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu", "optionEnabled", 1, true)
    Else
        UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionText", 1, "Say (look at NPC)")
        UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu", "optionEnabled", 1, false)
    EndIf
    UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionLabelText", 1, "Say")

    ; 2 (lower-left) - Think (literal NPC thought; needs a crosshair NPC)
    If haveNpc
        UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionText", 2, "Think (as " + cname + ")")
        UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu", "optionEnabled", 2, true)
    Else
        UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionText", 2, "Think (look at NPC)")
        UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu", "optionEnabled", 2, false)
    EndIf
    UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionLabelText", 2, "Think")

    ; 4 (upper-right) - Narrate
    UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionText", 4, "Narrate a scene event")
    UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionLabelText", 4, "Narrate")
    UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu", "optionEnabled", 4, true)

    ; 5 (right) - Transform (crosshair NPC speaks an LLM rephrase; needs a crosshair NPC)
    If haveNpc
        UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionText", 5, "Transform (as " + cname + ")")
        UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu", "optionEnabled", 5, true)
    Else
        UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionText", 5, "Transform (look at NPC)")
        UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu", "optionEnabled", 5, false)
    EndIf
    UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionLabelText", 5, "Transform")

    ; 6 (lower-right) - Sleep-talk on the crosshair NPC (context label)
    If haveNpc && (stf != None) && cross.IsInFaction(stf)
        UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionText", 6, "Wake " + cname)
        UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu", "optionEnabled", 6, true)
    ElseIf haveNpc
        UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionText", 6, "Sleep-talk " + cname)
        UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu", "optionEnabled", 6, true)
    Else
        UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionText", 6, "Sleep-talk (look at NPC)")
        UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu", "optionEnabled", 6, false)
    EndIf
    UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionLabelText", 6, "Sleep-talk")

    ; 3 (bottom) - Deep Sleep on the crosshair NPC (context label)
    If haveNpc && (slf != None) && cross.IsInFaction(slf)
        UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionText", 3, "Wake " + cname)
        UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu", "optionEnabled", 3, true)
    ElseIf haveNpc
        UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionText", 3, "Deep Sleep " + cname)
        UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu", "optionEnabled", 3, true)
    Else
        UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionText", 3, "Deep Sleep (look at NPC)")
        UIExtensions.SetMenuPropertyIndexBool("UIWheelMenu", "optionEnabled", 3, false)
    EndIf
    UIExtensions.SetMenuPropertyIndexString("UIWheelMenu", "optionLabelText", 3, "Deep Sleep")

    Int sel = UIExtensions.OpenMenu("UIWheelMenu")
    If sel == 7
        ToggleDirector()
    ElseIf sel == 0
        PromptSpeak()
    ElseIf sel == 1
        Say()
    ElseIf sel == 2
        Think()
    ElseIf sel == 4
        Narrate()
    ElseIf sel == 5
        Transform()
    ElseIf sel == 6
        SleeptalkCrosshair()
    ElseIf sel == 3
        AsleepCrosshair()
    EndIf
EndFunction
