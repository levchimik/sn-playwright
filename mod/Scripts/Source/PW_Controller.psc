Scriptname PW_Controller extends ReferenceAlias
{Playwright controller (v0.7). The PrismaUI panel (SNPlaywright.dll) is the sole
 control surface — it forwards button + keyboard actions here as the PW_PrismaCommand
 ModEvent. The only registered hotkey is the panel toggle (MCM-rebindable, optionally
 gated behind a modifier). Action functions are public so the MCM Status page and the
 panel bridge can invoke them directly.}

; --- Rebindable key (DXScanCode). Defaults to F11 (0x57); rebind in the MCM. ---
Int Property PrismaKey        = 87 Auto ; open/close the PrismaUI panel (87 = DXScanCode F11; needs SNPlaywright.dll + Prisma UI)

; --- Modifier gate for the panel toggle. ---
Int  Property ModifierKey     = 42 Auto    ; DXScanCode held to arm the panel key (42 = LShift; -1/0 = none)
Bool Property RequireModifier = true Auto  ; if true, the panel key only fires while ModifierKey is held

; --- Options. ---
Bool Property SendToBed = false Auto ; if true, EITHER sleep mode walks the NPC to the nearest bed (via SeverActions) to sleep there
Bool Property NpcSayVerbatim = true Auto ; NPC Say w/ Transform OFF: false = literal (dialogue text/memory, not voiced), true = verbatim (voiced via narration). Default: verbatim.

; --- Sleep-talk murmuring (ambient dream-speech channel; does NOT route through the
;     speaker selector, so no one is ever pulled into a conversation with the sleeper). ---
Bool  Property SleeptalkMurmur = true Auto  ; if true, sleep-talkers occasionally mutter dream fragments aloud
Float Property MurmurInterval  = 30.0 Auto  ; seconds between murmur checks
Float Property MurmurChance    = 0.35 Auto  ; per-check chance each nearby sleep-talker murmurs

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
; Returns true if the actor was actually sent to a bed. Never the player; needs
; SeverActions and a bed. When no bed is found the actor just sleeps where they stand.
Bool Function SendActorToBed(Actor t)
    If t == Game.GetPlayer()
        Return false
    EndIf
    If Game.GetModByName("SeverActions.esp") == 255
        Debug.Notification("Send to bed needs SeverActions installed")
        Return false
    EndIf
    ObjectReference bed = FindNearestBed(t)
    If !bed
        Debug.Notification("No bed nearby for " + t.GetDisplayName())
        Return false
    EndIf
    ; Pass the PLACED reference's FormID (as hex) so SeverActions resolves THIS exact
    ; bed directly. Passing the base object's FormID instead forces a BaseID->RefID
    ; fallback limited to 500u of the actor, which silently fails for any bed farther
    ; than that and leaves the NPC standing while our notification still fires.
    SeverActions_Furniture.UseFurniture_Global_Execute(t, FormIDToHex(bed.GetFormID()))
    Debug.Notification(t.GetDisplayName() + " is heading to bed")
    Return true
EndFunction

; Position a freshly-asleep actor. If "Send to bed" is on we walk them to the
; nearest bed (never the player); otherwise they sleep where they stand. (A
; ragdoll/paralysis fallback was tried but never held a HELD ragdoll in this load
; order — see KNOWLEDGEBASE — so it was scrapped in favour of bed-only.)
Function PlaceSleeper(Actor t)
    If SendToBed
        SendActorToBed(t)
    EndIf
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
    RegisterPrismaEvents()
EndEvent

Event OnPlayerLoadGame()
    RegisterKeys()
    RegisterDecorators()
    RegisterPrismaEvents()
EndEvent

; The PrismaUI panel lives in SNPlaywright.dll. It enumerates the nearby cast and
; pushes it to the web view itself; we only need to receive the action payloads it
; forwards. The DLL fires "PW_PrismaCommand" as a simple SKSE ModEvent (strArg =
; "action|targetId|text") which this alias handles. Re-register every load -- mod
; event registrations don't survive a reload.
Function RegisterPrismaEvents()
    RegisterForModEvent("PW_PrismaCommand", "OnPrismaCommand")
EndFunction

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

; Only the panel-toggle key is registered now (the radial wheel and the per-action
; hotkeys were retired in v0.7 — the PrismaUI panel covers everything).
Function RegisterKeys()
    UnregisterForAllKeys()
    If PrismaKey > 0
        RegisterForKey(PrismaKey)
    EndIf
EndFunction

Event OnKeyDown(Int keyCode)
    ; Optional modifier gate (SeverActions pattern): ignore the panel key unless the
    ; configured modifier is held. The panel toggle must fire even while the panel is
    ; OPEN (which puts the game in menu mode), so there is no menu-mode guard.
    If RequireModifier && ModifierKey > 0 && !Input.IsKeyPressed(ModifierKey)
        Return
    EndIf
    If keyCode == PrismaKey
        TogglePrismaPanel()
    EndIf
EndEvent

; Ask SNPlaywright.dll to open/close the PrismaUI panel. The DLL listens for this
; simple SKSE ModEvent (its C++ ModCallbackEvent sink). No-op if the DLL/Prisma UI
; isn't installed. Lets the panel key live in the MCM like every other binding.
Function TogglePrismaPanel()
    SendModEvent("PW_PrismaToggle", "", 0.0)
EndFunction

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
        SendModEvent("PW_PrismaDirector", "", 0.0)
    Else
        ; --- Director Mode ON: leave the scene ---
        pl.AddToFaction(blf)
        _pdWasEnabled = SkyrimNetApi.GetConfigBool("PlayerDialogue", "enabled", true)
        SkyrimNetApi.PatchConfig("PlayerDialogue", "{\"enabled\": false}")
        Debug.Notification("Director Mode: ON - you have left the scene")
        SendModEvent("PW_PrismaDirector", "", 1.0)
    EndIf
EndFunction

; ------------------------------------------------------------------ deep sleep (unconscious)
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
        ; Tell SkyrimNet so nearby NPCs perceive the state change. RegisterPersistentEvent
        ; informs without triggering a dialogue reaction, so it adds no new leak vector.
        SkyrimNetApi.RegisterPersistentEvent(t.GetDisplayName() + " has fallen into a deep, unresponsive sleep.", t, None)
        Debug.Notification(t.GetDisplayName() + " is in a deep sleep")
        ; Position LAST so the walk-to-bed never delays the notification above. Only when
        ; coming from awake; a deep<->talk switch keeps whatever bed state they already had.
        If !wasTalk
            PlaceSleeper(t)
        EndIf
    EndIf
EndFunction

; ------------------------------------------------------------------ sleep-talk
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
        StartMurmurLoop()
        SkyrimNetApi.RegisterPersistentEvent(t.GetDisplayName() + " has drifted into a restless sleep, murmuring softly.", t, None)
        Debug.Notification(t.GetDisplayName() + " is now sleep-talking")
        ; Position LAST so the walk-to-bed never delays the notification above. Only when
        ; coming from awake; a deep<->talk switch keeps whatever bed state they already had.
        If !wasDeep
            PlaceSleeper(t)
        EndIf
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

; ------------------------------------------------------------------ narrate
; Voice a scene event through a nearby NPC. preferred = the NPC to voice it (the panel's
; selected target); None falls back to a random nearby NPC. Works in or out of Director
; Mode (DirectNarration with an NPC originator + no target).
Function NarrateText(String narration, Actor preferred)
    Actor pl = Game.GetPlayer()
    Actor speaker = preferred
    Int tries = 0
    While (!speaker || speaker == pl) && tries < 6
        speaker = Game.FindRandomActorFromRef(pl, 1500.0)
        tries += 1
    EndWhile
    If !speaker || speaker == pl
        Debug.Notification("No NPC nearby to perceive the narration")
        Return
    EndIf
    If narration != ""
        SkyrimNetApi.DirectNarration(narration, speaker, None)
        Debug.Notification("Narrated via " + speaker.GetDisplayName())
    EndIf
EndFunction

; System/meta event (panel "System" button): inject a neutral context note that NPCs
; become aware of for context but do NOT react to (RegisterPersistentEvent). Surfaces in
; the log as a "System" entry. When a target is selected the note is associated with them
; (targetActor); otherwise it's a general world/system note.
Function SystemEvent(String content, Actor target)
    If content != ""
        SkyrimNetApi.RegisterPersistentEvent(content, None, target)
        Debug.Notification("System event sent")
    EndIf
EndFunction

; ------------------------------------------------------------------ say (panel speaker/target)
; Panel Say with the speaker/target pairing. The speaker utters the line; if a distinct
; target is also selected, it's addressed TO them. The PLAYER is the default speaker, so a lone
; selection is the TARGET (you say it to them); pick a 2nd actor to make THAT one the speaker.
;
; PLAYER speaker: Transform ON -> TransformDialogue (LLM rephrases, voiced); Transform OFF ->
;   the literal line as the player's dialogue (text/memory).
; NPC speaker:
;   Transform ON              -> DirectNarration "<Speaker> says to <Target>, rephrased: ..." so
;                                the LLM puts the gist in the NPC's own voice and they speak it.
;   Transform OFF + Verbatim   -> DirectNarration "... verbatim: ..." so the NPC voices the EXACT
;     (NpcSayVerbatim == true)    line aloud.
;   Transform OFF + Literal    -> RegisterDialogue: inject the exact words as dialogue text/memory,
;     (NpcSayVerbatim == false)   NOT voiced (SkyrimNet only TTS's LLM-generated lines).
Function SayPaired(Actor speaker, Actor listener, String line, Bool xform)
    If line == ""
        Return
    EndIf
    Actor pl = Game.GetPlayer()
    Actor talker = speaker
    If !talker
        talker = pl              ; no explicit speaker -> the player is the default speaker
    EndIf
    If listener == talker
        listener = None          ; don't address yourself
    EndIf

    ; ---- Player speaker ----
    If talker == pl
        If xform
            SkyrimNetApi.TransformDialogue(line)   ; LLM rephrases; the player speaks it (voiced)
            Debug.Notification("You say it (rephrased)")
        Else
            ; Transform off: record the exact line to context, then voice it aloud. RegisterDialogue
            ; only logs it (silent); TriggerPlayerTTS sends the literal line straight to TTS (direct,
            ; no LLM), so the player actually pronounces it.
            If listener
                SkyrimNetApi.RegisterDialogueToListener(talker, listener, line)
            Else
                SkyrimNetApi.RegisterDialogue(talker, line)
            EndIf
            SkyrimNetApi.TriggerPlayerTTS(line)
            Debug.Notification("You say it aloud")
        EndIf
        Return
    EndIf

    ; ---- NPC speaker ----
    If xform
        ; Rephrased: narrate it so the LLM delivers the gist in the NPC's own voice (voiced).
        SkyrimNetApi.DirectNarration(SayNarration(talker, listener, line, "rephrased"), talker, listener)
        Debug.Notification(talker.GetDisplayName() + " says it (rephrased)")
    ElseIf NpcSayVerbatim
        ; Verbatim mode: narrate it so the NPC voices the EXACT line aloud.
        SkyrimNetApi.DirectNarration(SayNarration(talker, listener, line, "verbatim"), talker, listener)
        Debug.Notification(talker.GetDisplayName() + " says it (verbatim)")
    ElseIf listener
        ; Literal mode: inject the exact words as dialogue (text/memory), addressed to the target.
        SkyrimNetApi.RegisterDialogueToListener(talker, listener, line)
        Debug.Notification(talker.GetDisplayName() + " -> " + listener.GetDisplayName())
    Else
        SkyrimNetApi.RegisterDialogue(talker, line)
        Debug.Notification(talker.GetDisplayName() + " says it")
    EndIf
EndFunction

; Stage-direction narration for a voiced NPC line. mode = "verbatim" (deliver the line
; exactly) or "rephrased" (LLM puts it in the NPC's own words). The line is quoted so the
; LLM treats it as the content to deliver, not a scene to paraphrase freely.
String Function SayNarration(Actor talker, Actor listener, String line, String mode)
    If listener
        Return talker.GetDisplayName() + " says to " + listener.GetDisplayName() + ", " + mode + ": \"" + line + "\""
    EndIf
    Return talker.GetDisplayName() + " says, " + mode + ": \"" + line + "\""
EndFunction

; ------------------------------------------------------------------ think (literal NPC thought)
; Inject a literal, persistent, unvoiced thought into an NPC, verbatim (no LLM). The
; npc_thoughts schema requires both `npc_name` and `thoughts`, so we hand the structured
; data to RegisterEvent as a JSON object in the content arg — a bare string only fills
; `thoughts` and logs "Missing required field 'npc_name'". JSON is escaped via SeverActions'
; native helper so quotes/backslashes in the thought are safe. Used by the PrismaUI panel.
Function ThinkTo(Actor t, String thought)
    If t && thought != ""
        String data = "{\"npc_name\":\"" + SeverActionsNative.EscapeJsonString(t.GetDisplayName()) + "\",\"thoughts\":\"" + SeverActionsNative.EscapeJsonString(thought) + "\"}"
        SkyrimNetApi.RegisterEvent("npc_thoughts", data, t, None)
        Debug.Notification(t.GetDisplayName() + " thinks it")
    EndIf
EndFunction

; LLM counterpart of ThinkTo (panel Transform-mode ON): hand the gist to SkyrimNet,
; which generates an unvoiced thought in the NPC's own voice and persists it privately
; (GenerateNPCThought). Skips silently for the player or a dead/sleeping actor.
Function ThinkLLM(Actor t, String hint)
    If t && hint != ""
        SkyrimNetApi.GenerateNPCThought(t, hint)
        Debug.Notification(t.GetDisplayName() + " thinks (LLM)")
    EndIf
EndFunction

; ------------------------------------------------------------------ transform (NPC line via LLM)
; Core (used by the PrismaUI panel). Hand the gist of what you want the NPC to say; the LLM
; turns it into a line in THAT NPC's voice and the NPC speaks it (the LLM counterpart of
; Say). Fires from the NPC, not the player: DirectNarration with the NPC as originator.
Function TransformTo(Actor t, String line)
    If t && line != ""
        SkyrimNetApi.DirectNarration(line, t, None)
        Debug.Notification(t.GetDisplayName() + " (transformed)")
    EndIf
EndFunction

; ------------------------------------------------------------------ prompt (no text)
; Nudge a character to speak without typing anything:
;   * A specific NPC -> DirectNarration("", npc): an empty-content (ephemeral) direct
;     narration forces THAT originator to respond immediately. This is the same call the
;     sleep-talk murmur loop uses, and it's the documented "trigger immediate response"
;     path. (TriggerContinueNarration() was unreliable here — it hands off to SkyrimNet's
;     speaker selector, which frequently picks no one, so the prompted NPC never spoke.)
;   * The PLAYER -> TriggerPlayerDialogue(): SkyrimNet's autonomous player-dialogue
;     ("auto-roleplay") path.
;   * No specific target -> TriggerContinueNarration(): let the selector continue the scene.
; preferred = the nudge target (the panel's selected speaker/target).
Function PromptActor(Actor preferred)
    If preferred == Game.GetPlayer()
        SkyrimNetApi.TriggerPlayerDialogue()
        Debug.Notification("Nudging you to speak (auto-roleplay)")
    ElseIf preferred
        SkyrimNetApi.DirectNarration("", preferred, None)   ; force THIS npc to respond now
        Debug.Notification("Prompted " + preferred.GetDisplayName() + " to speak")
    Else
        SkyrimNetApi.TriggerContinueNarration()             ; no target -> selector continues the scene
        Debug.Notification("Continuing the scene...")
    EndIf
EndFunction

; ------------------------------------------------------------------ PrismaUI panel
; Action payload from SNPlaywright.dll: "action|targetId|text".
;   targetId: "player", a "0x"-prefixed runtime FormID, or "" (no target).
;   text: free text (may itself contain '|', so it's everything after field 2).
; Actions map onto the shared action cores below.
Function OnPrismaCommand(String eventName, String strArg, Float numArg, Form akSender)
    ; Panel payload: "action|targetId|transform|speakerId|text" (text is everything after
    ; field 4, so it may itself contain '|'). transform == "1" routes the text actions
    ; through the LLM (rephrased / voiced / generated); otherwise the literal/verbatim path
    ; is used. Quick actions (prompt/sleep/...) send only "action|targetId|", so transform,
    ; speaker, and text come back empty — they ignore them.
    ;
    ; Speaker vs target: Say uses the speaker->target pairing (speaker says the line TO the
    ; target). Every other text action ignores the pairing and acts on the "primary" actor
    ; (the speaker if one is set, otherwise the target).
    String action = PipeField(strArg, 0)
    String targetTok = PipeField(strArg, 1)
    Bool xform = (PipeField(strArg, 2) == "1")
    String speakerTok = PipeField(strArg, 3)
    String text = PipeRest(strArg, 4)
    Actor t = ResolveTarget(targetTok)
    Actor sp = ResolveTarget(speakerTok)
    Actor pl = Game.GetPlayer()
    Actor primary = sp
    If !primary
        primary = t
    EndIf
    ; Diagnostic (v0.6): confirms the JS->C++->Papyrus bridge delivered the click.
    Debug.Trace("[Playwright] OnPrismaCommand: " + strArg, 0)

    If action == "director"
        ToggleDirector()
    ElseIf action == "whisper"
        SkyrimNetApi.TriggerToggleWhisperMode()   ; global: swaps interaction.maxDistance (normal <-> whisper)
    ElseIf action == "interrupt"
        SkyrimNetApi.TriggerInterruptDialogue()   ; global: cut off in-progress NPC speech + pending generation queues
    ElseIf action == "continuous"
        SkyrimNetApi.TriggerToggleContinuousMode()  ; global: autonomous-scene mode (needs GameMaster enabled)
    ElseIf action == "prompt"
        PromptActor(primary)
    ElseIf action == "narrate"
        NarrateText(text, primary)      ; a nearby NPC voices/reacts to the scene event (LLM)
    ElseIf action == "system"
        SystemEvent(text, primary)      ; neutral system/context note; no LLM reaction
    ElseIf action == "say"
        SayPaired(sp, t, text, xform)   ; speaker -> target (verbatim or LLM-voiced)
    ElseIf action == "transform"
        If primary && primary != pl
            TransformTo(primary, text)
        EndIf
    ElseIf action == "think"
        If primary && primary != pl
            If xform
                ThinkLLM(primary, text) ; LLM generates the thought from your gist
            Else
                ThinkTo(primary, text)  ; literal verbatim thought
            EndIf
        EndIf
    ElseIf action == "sleep"
        If t
            PrismaSleep(t)
        EndIf
    ElseIf action == "sleeptalk"
        If t
            PrismaSleeptalk(t)
        EndIf
    ElseIf action == "wake"
        If t
            PrismaWake(t)
        EndIf
    EndIf
EndFunction

; Idempotent sleep-state setters for the panel (the panel has explicit Deep Sleep /
; Sleep-talk / Wake buttons). They lean on the existing toggles so all the deaf-window /
; walk-to-bed / mutual-exclusion logic is shared and there's one source of truth.
Function PrismaSleep(Actor t)
    Faction slf = GetAsleepFaction()
    If slf && !t.IsInFaction(slf)
        ToggleAsleepActor(t)   ; awake/sleep-talk -> deep sleep
    EndIf
EndFunction

Function PrismaSleeptalk(Actor t)
    Faction stf = GetSleeptalkFaction()
    If stf && !t.IsInFaction(stf)
        ToggleSleeptalkActor(t)   ; awake/deep-sleep -> sleep-talk
    EndIf
EndFunction

Function PrismaWake(Actor t)
    Faction slf = GetAsleepFaction()
    Faction stf = GetSleeptalkFaction()
    If slf && t.IsInFaction(slf)
        ToggleAsleepActor(t)      ; wakes
    ElseIf stf && t.IsInFaction(stf)
        ToggleSleeptalkActor(t)   ; wakes
    EndIf
EndFunction

; "player" -> the player; "0x...." -> resolve the runtime FormID (SKSE GetFormEx
; handles the full unsigned 32-bit range, incl. ESL FE.. ids); "" -> None.
Actor Function ResolveTarget(String token)
    If token == ""
        Return None
    EndIf
    If token == "player" || token == "Player"
        Return Game.GetPlayer()
    EndIf
    Return Game.GetFormEx(HexToInt(token)) as Actor
EndFunction

; Parse a "0x"-prefixed (or bare) hex string to an Int. Two's-complement wrap is
; intentional: FormIDs above 0x7FFFFFFF come back as a negative Int whose bit
; pattern still equals the FormID, which is exactly what GetFormEx wants.
Int Function HexToInt(String s)
    If StringUtil.GetLength(s) >= 2 && StringUtil.SubString(s, 0, 2) == "0x"
        s = StringUtil.SubString(s, 2)
    EndIf
    String lower = "0123456789abcdef"
    String upper = "0123456789ABCDEF"
    Int val = 0
    Int i = 0
    Int n = StringUtil.GetLength(s)
    While i < n
        String ch = StringUtil.SubString(s, i, 1)
        Int d = StringUtil.Find(lower, ch)
        If d < 0
            d = StringUtil.Find(upper, ch)
        EndIf
        If d >= 0
            val = val * 16 + d
        EndIf
        i += 1
    EndWhile
    Return val
EndFunction

; Nth '|'-delimited field (0-indexed); "" if absent.
String Function PipeField(String data, Int index)
    Int pos = 0
    Int field = 0
    Int len = StringUtil.GetLength(data)
    While field < index && pos < len
        Int p = StringUtil.Find(data, "|", pos)
        If p < 0
            Return ""
        EndIf
        pos = p + 1
        field += 1
    EndWhile
    Int nextP = StringUtil.Find(data, "|", pos)
    If nextP < 0
        Return StringUtil.SubString(data, pos)
    EndIf
    Return StringUtil.SubString(data, pos, nextP - pos)
EndFunction

; Everything from the Nth field onward (so trailing text keeps any '|' it contains).
String Function PipeRest(String data, Int index)
    Int pos = 0
    Int field = 0
    Int len = StringUtil.GetLength(data)
    While field < index && pos < len
        Int p = StringUtil.Find(data, "|", pos)
        If p < 0
            Return ""
        EndIf
        pos = p + 1
        field += 1
    EndWhile
    Return StringUtil.SubString(data, pos)
EndFunction
