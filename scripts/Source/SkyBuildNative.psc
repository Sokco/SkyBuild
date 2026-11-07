Scriptname SkyBuildNative Hidden

; Register a clean, nonrespawning Container as a display rack with model-authored slots.
; The loaded rack model supplies the available display pivots.
Bool Function ConfigureWeaponRack(ObjectReference akRack) Global Native

; Remove runtime visual geometry only. Inventory is never removed here.
Function ForgetWeaponRack(ObjectReference akRack) Global Native
