# Menu selection with a pad

This is the launcher option **Gamepad menu selection** (Launcher > Controls, off by default, saved as `gamepad_menu`).
With it off the menus behave exactly as the original hardware does.

Circuit select and transmission select are steered with the wheel. The game highlights the item the wheel reads at
(left / centre / right) and steps while the wheel is held outside a narrow band. A pad cannot hold a wheel, so
`Controls::sample` holds it for the pad: one push moves the selection one step and the wheel stays there until the next
push, as on the Saturn and Dreamcast ports.

Measured on the Revision A ROM set with temporary RAM snapshots (2 minutes of menus, 4 games):

* The menu steering reads the ADC with its centre at **0x8e**, not 0x80. A centred pad at 0x80 reads as "left" and keeps stepping.
* `0x501468` is the highlighted item (0, 1 or 2). In circuit select centre highlights track 2 and right track 3; in the two-option transmission select centre and left highlight automatic and right highlights manual.
* A new menu screen shows as a change in `0x504394` (0 to a fixed value) and `0x5043b8` (396 to 388), once, as transmission select opens. Both hold the same values in every recording. They also change once as the circuit menu itself loads, so changes in the first 20 frames of the menus are ignored.
* Without the reset below, picking track 3 left the wheel held right, so the transmission menu opened on the second option (manual).

Behaviour: every new menu opens on its first entry (the held wheel goes back to the left; a push still held is not a new
push), and transmission select takes left or right at once (centre would otherwise take two pushes to reach manual).
