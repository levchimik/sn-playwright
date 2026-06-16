Scriptname PW_MCM extends SKI_ConfigBase
{SkyUI MCM for Playwright. Rebinds the five SND hotkeys and the
 modifier on the PW_Controller alias, and exposes a small Status page with
 quick-action buttons. The controller is the quest's first (and only) alias.}

PW_Controller _ctrl

; cached option IDs (Controls page)
Int _oiWheel
Int _oiToggle
Int _oiNarrate
Int _oiPrompt
Int _oiSay
Int _oiThink
Int _oiTransform
Int _oiAsleep
Int _oiAsleepSelf
Int _oiSleeptalk
Int _oiSleeptalkSelf
Int _oiReqMod
Int _oiModKey
Int _oiSendBed
Int _oiMurmur
Int _oiMurmurInt
Int _oiMurmurChance

; cached option IDs (Status page)
Int _oiDirBtn
Int _oiSleepSelfBtn
Int _oiTalkSelfBtn

PW_Controller Function GetCtrl()
    If !_ctrl
        ; The controller lives on the OTHER quest (PW_DirectorQuest, 0x800), on its
        ; single player alias. This MCM is on its own quest (0x802) so it reliably
        ; initializes when added mid-save.
        Quest cq = Game.GetFormFromFile(0x800, "SNPlaywright.esp") as Quest
        If cq
            _ctrl = cq.GetNthAlias(0) as PW_Controller
        EndIf
    EndIf
    Return _ctrl
EndFunction

Function OnConfigInit()
    ModName = "Playwright"
    Pages = new String[2]
    Pages[0] = "Controls"
    Pages[1] = "Status"
EndFunction

; ------------------------------------------------------------------ page build
Function OnPageReset(String page)
    PW_Controller c = GetCtrl()
    If !c
        AddHeaderOption("Controller not found", OPTION_FLAG_DISABLED)
        Return
    EndIf

    If page == "Status"
        SetCursorFillMode(TOP_TO_BOTTOM)
        AddHeaderOption("Status", 0)
        If c.IsDirectorOn()
            AddTextOption("Director Mode", "ON", OPTION_FLAG_DISABLED)
        Else
            AddTextOption("Director Mode", "OFF", OPTION_FLAG_DISABLED)
        EndIf

        AddHeaderOption("Quick Actions", 0)
        If c.IsDirectorOn()
            _oiDirBtn = AddTextOption("Director Mode", "EXIT scene", 0)
        Else
            _oiDirBtn = AddTextOption("Director Mode", "ENTER scene", 0)
        EndIf
        _oiSleepSelfBtn = AddTextOption("Deep Sleep: Self", "toggle", 0)
        _oiTalkSelfBtn = AddTextOption("Sleep-talk: Self", "toggle", 0)
    Else
        ; default + "Controls" page
        SetCursorFillMode(TOP_TO_BOTTOM)
        AddHeaderOption("Hotkeys", 0)
        _oiWheel      = AddKeyMapOption("Open Wheel", c.WheelKey, 0)
        _oiToggle     = AddKeyMapOption("Toggle Director Mode", c.ToggleKey, 0)
        _oiNarrate    = AddKeyMapOption("Narrate (text)", c.NarrateKey, 0)
        _oiPrompt     = AddKeyMapOption("Prompt to Speak (no text)", c.PromptKey, 0)
        _oiSay        = AddKeyMapOption("Say (literal, crosshair NPC)", c.SayKey, 0)
        _oiThink      = AddKeyMapOption("Think (literal, crosshair NPC)", c.ThinkKey, 0)
        _oiTransform  = AddKeyMapOption("Transform (crosshair NPC via LLM)", c.TransformKey, 0)
        _oiAsleep       = AddKeyMapOption("Deep Sleep: Crosshair NPC", c.AsleepKey, 0)
        _oiAsleepSelf   = AddKeyMapOption("Deep Sleep: Self", c.AsleepSelfKey, 0)
        _oiSleeptalk    = AddKeyMapOption("Sleep-talk: Crosshair NPC", c.SleeptalkKey, 0)
        _oiSleeptalkSelf = AddKeyMapOption("Sleep-talk: Self", c.SleeptalkSelfKey, 0)

        AddHeaderOption("Modifier", 0)
        _oiReqMod = AddToggleOption("Require modifier", c.RequireModifier, 0)
        _oiModKey = AddKeyMapOption("Modifier key", c.ModifierKey, 0)

        AddHeaderOption("Options", 0)
        _oiSendBed = AddToggleOption("Send to bed (while sleeping)", c.SendToBed, 0)
        _oiMurmur  = AddToggleOption("Sleep-talk murmuring", c.SleeptalkMurmur, 0)
        Int murmurFlag = 0
        If !c.SleeptalkMurmur
            murmurFlag = OPTION_FLAG_DISABLED
        EndIf
        _oiMurmurInt    = AddSliderOption("  Murmur interval (s)", c.MurmurInterval, "{0}", murmurFlag)
        _oiMurmurChance = AddSliderOption("  Murmur chance", c.MurmurChance, "{2}", murmurFlag)
    EndIf
EndFunction

; ------------------------------------------------------------------ keymaps
Function OnOptionKeyMapChange(Int option, Int keyCode, String conflictControl, String conflictName)
    PW_Controller c = GetCtrl()
    If !c
        Return
    EndIf

    Bool ok = true
    If conflictControl != ""
        String msg = "This key is already mapped to:\n" + conflictControl
        If conflictName != ""
            msg += " (" + conflictName + ")"
        EndIf
        msg += "\n\nAssign anyway?"
        ok = ShowMessage(msg, true, "Yes", "No")
    EndIf
    If !ok
        Return
    EndIf

    If option == _oiWheel
        c.WheelKey = keyCode
    ElseIf option == _oiToggle
        c.ToggleKey = keyCode
    ElseIf option == _oiNarrate
        c.NarrateKey = keyCode
    ElseIf option == _oiPrompt
        c.PromptKey = keyCode
    ElseIf option == _oiSay
        c.SayKey = keyCode
    ElseIf option == _oiThink
        c.ThinkKey = keyCode
    ElseIf option == _oiTransform
        c.TransformKey = keyCode
    ElseIf option == _oiAsleep
        c.AsleepKey = keyCode
    ElseIf option == _oiAsleepSelf
        c.AsleepSelfKey = keyCode
    ElseIf option == _oiSleeptalk
        c.SleeptalkKey = keyCode
    ElseIf option == _oiSleeptalkSelf
        c.SleeptalkSelfKey = keyCode
    ElseIf option == _oiModKey
        c.ModifierKey = keyCode
    Else
        Return
    EndIf
    SetKeyMapOptionValue(option, keyCode, false)
    c.RegisterKeys()
EndFunction

; ------------------------------------------------------------------ toggles / buttons
Function OnOptionSelect(Int option)
    PW_Controller c = GetCtrl()
    If !c
        Return
    EndIf
    If option == _oiReqMod
        c.RequireModifier = !c.RequireModifier
        SetToggleOptionValue(option, c.RequireModifier, false)
    ElseIf option == _oiSendBed
        c.SendToBed = !c.SendToBed
        SetToggleOptionValue(option, c.SendToBed, false)
    ElseIf option == _oiMurmur
        c.SleeptalkMurmur = !c.SleeptalkMurmur
        SetToggleOptionValue(option, c.SleeptalkMurmur, false)
        If c.SleeptalkMurmur
            c.StartMurmurLoop() ; kick the loop now in case a sleep-talker is already nearby
        EndIf
        ForcePageReset() ; refresh enabled/disabled state of the murmur sliders
    ElseIf option == _oiDirBtn
        c.ToggleDirector()
        ForcePageReset()
    ElseIf option == _oiSleepSelfBtn
        c.AsleepSelf()
    ElseIf option == _oiTalkSelfBtn
        c.SleeptalkSelf()
    EndIf
EndFunction

; ------------------------------------------------------------------ sliders
Function OnOptionSliderOpen(Int option)
    PW_Controller c = GetCtrl()
    If !c
        Return
    EndIf
    If option == _oiMurmurInt
        SetSliderDialogStartValue(c.MurmurInterval)
        SetSliderDialogDefaultValue(30.0)
        SetSliderDialogRange(5.0, 120.0)
        SetSliderDialogInterval(5.0)
    ElseIf option == _oiMurmurChance
        SetSliderDialogStartValue(c.MurmurChance)
        SetSliderDialogDefaultValue(0.35)
        SetSliderDialogRange(0.0, 1.0)
        SetSliderDialogInterval(0.05)
    EndIf
EndFunction

Function OnOptionSliderAccept(Int option, Float value)
    PW_Controller c = GetCtrl()
    If !c
        Return
    EndIf
    If option == _oiMurmurInt
        c.MurmurInterval = value
        SetSliderOptionValue(option, value, "{0}", false)
    ElseIf option == _oiMurmurChance
        c.MurmurChance = value
        SetSliderOptionValue(option, value, "{2}", false)
    EndIf
EndFunction

; ------------------------------------------------------------------ info text
Function OnOptionHighlight(Int option)
    If option == _oiReqMod
        SetInfoText("If ON, SND hotkeys only fire while the modifier key is held. Prevents accidental presses.")
    ElseIf option == _oiModKey
        SetInfoText("Press a key to set the modifier that must be held to arm the SND hotkeys (default Left Shift).")
    ElseIf option == _oiSendBed
        SetInfoText("If ON, entering Deep Sleep OR Sleep-talk walks the NPC to the nearest bed (via SeverActions) to sleep there. Needs SeverActions installed and a bed within ~58m; otherwise they sleep where they stand.")
    ElseIf option == _oiMurmur
        SetInfoText("If ON, Sleep-talk actors occasionally mutter incoherent dream fragments aloud. Uses an ambient channel that never pulls others into a conversation with the sleeper. Deep Sleep stays silent.")
    ElseIf option == _oiMurmurInt
        SetInfoText("Seconds between murmur checks for each nearby sleep-talker. Higher = rarer murmurs (and fewer LLM/voice calls). Default 30.")
    ElseIf option == _oiMurmurChance
        SetInfoText("Chance (0-1) that a nearby sleep-talker murmurs on each check. Lower = sparser. Default 0.35. Each murmur costs one LLM + voice generation.")
    ElseIf option == _oiWheel
        SetInfoText("Opens the radial wheel: Enter/Exit Scene, Narrate, Sleep/Wake crosshair NPC.")
    ElseIf option == _oiToggle
        SetInfoText("Director Mode: removes you from the scene so nearby NPCs talk among themselves.")
    ElseIf option == _oiNarrate
        SetInfoText("Type narration; a nearby NPC voices it as a general scene event to everyone present. Works in or out of Director Mode.")
    ElseIf option == _oiPrompt
        SetInfoText("No-text nudge: prompts the crosshair NPC (or a nearby one) to speak/continue the scene.")
    ElseIf option == _oiSay
        SetInfoText("Type a line; it's injected as the crosshair NPC's exact words (subtitle + memory), VERBATIM, no LLM. NOT voiced — SkyrimNet can't TTS an arbitrary literal line. Use Transform for a voiced (LLM) line.")
    ElseIf option == _oiThink
        SetInfoText("Type a thought; the crosshair NPC thinks it (private, unvoiced, colors their behavior). LLM-mediated, so wording may be lightly rephrased. Skips dead/unconscious/sleeping NPCs.")
    ElseIf option == _oiTransform
        SetInfoText("Type the gist of what you want the crosshair NPC to say; the LLM phrases it in THEIR voice and they speak it (the LLM version of Say). Look at the NPC first.")
    ElseIf option == _oiAsleep
        SetInfoText("Deep Sleep: the crosshair NPC goes unconscious — deaf, unresponsive, and others see them as out cold. Cannot be picked to speak.")
    ElseIf option == _oiAsleepSelf
        SetInfoText("Deep Sleep on yourself: you stay visible but perceive nothing.")
    ElseIf option == _oiSleeptalk
        SetInfoText("Sleep-talk: the crosshair NPC is deaf (perceives nothing) but may occasionally mutter incoherent dream-speech; others hear the murmuring.")
    ElseIf option == _oiSleeptalkSelf
        SetInfoText("Sleep-talk on yourself: you perceive nothing but may murmur in your sleep.")
    ElseIf option == _oiDirBtn
        SetInfoText("Enter or exit Director Mode now.")
    ElseIf option == _oiSleepSelfBtn
        SetInfoText("Toggle Deep Sleep on yourself.")
    ElseIf option == _oiTalkSelfBtn
        SetInfoText("Toggle Sleep-talk on yourself.")
    EndIf
EndFunction
