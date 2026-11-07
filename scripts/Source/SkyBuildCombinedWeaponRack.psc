Scriptname SkyBuildCombinedWeaponRack extends ObjectReference
{Container rack. Real items remain in inventory; authored RackSlot nodes provide multiple non-interactive displays.}

Bool warned = False

Event OnInit()
    ConfigureRack()
EndEvent

Event OnLoad()
    ConfigureRack()
EndEvent

Event OnUnload()
    SkyBuildNative.ForgetWeaponRack(Self)
EndEvent

Function ConfigureRack()
    ; Keep native Container activation. Also undo the old activator script's
    ; saved BlockActivation state if this script was upgraded on a reference.
    BlockActivation(False)
    If SkyBuildNative.ConfigureWeaponRack(Self)
        warned = False
    ElseIf !warned
        warned = True
        Debug.Notification("Rack needs a Container base, Respawn off, no starting items, and SkyBuild loaded.")
    EndIf
EndFunction
