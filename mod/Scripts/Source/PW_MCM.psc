Scriptname PW_MCM extends SKI_ConfigBase
{SkyUI MCM for Playwright. Binds the PrismaUI panel toggle (+ its modifier) on the
 PW_Controller alias, exposes the sleep options, and a small Status page with
 quick-action buttons. The controller is the quest's first (and only) alias.}

PW_Controller _ctrl

; cached option IDs (Controls page)
Int _oiPrisma
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
        AddHeaderOption("Panel", 0)
        _oiPrisma     = AddKeyMapOption("Open PrismaUI Panel", c.PrismaKey, 0)

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

    If option == _oiPrisma
        c.PrismaKey = keyCode
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
        SetInfoText("If ON, the panel toggle key only fires while the modifier key is held. Prevents accidental opens.")
    ElseIf option == _oiModKey
        SetInfoText("Press a key to set the modifier that must be held to arm the panel toggle key (default Left Shift).")
    ElseIf option == _oiSendBed
        SetInfoText("If ON, entering Deep Sleep OR Sleep-talk walks the NPC to the nearest bed (via SeverActions) to sleep there. Needs SeverActions installed and a bed within ~58m; otherwise they sleep where they stand.")
    ElseIf option == _oiMurmur
        SetInfoText("If ON, Sleep-talk actors occasionally mutter incoherent dream fragments aloud. Uses an ambient channel that never pulls others into a conversation with the sleeper. Deep Sleep stays silent.")
    ElseIf option == _oiMurmurInt
        SetInfoText("Seconds between murmur checks for each nearby sleep-talker. Higher = rarer murmurs (and fewer LLM/voice calls). Default 30.")
    ElseIf option == _oiMurmurChance
        SetInfoText("Chance (0-1) that a nearby sleep-talker murmurs on each check. Lower = sparser. Default 0.35. Each murmur costs one LLM + voice generation.")
    ElseIf option == _oiPrisma
        SetInfoText("Opens the PrismaUI control panel: nearby-NPC list + action buttons + text box, fully keyboard-drivable. This is the ONLY way to open the panel (default Shift+F11). Needs SNPlaywright.dll and the Prisma UI framework.")
    ElseIf option == _oiDirBtn
        SetInfoText("Enter or exit Director Mode now.")
    ElseIf option == _oiSleepSelfBtn
        SetInfoText("Toggle Deep Sleep on yourself.")
    ElseIf option == _oiTalkSelfBtn
        SetInfoText("Toggle Sleep-talk on yourself.")
    EndIf
EndFunction
