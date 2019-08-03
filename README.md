# Nintendo Switch Pro Controller - HID Linux Driver
## Features

* This driver fully enables normal controller usage both over bluetooth and USB.
* The gyroscope can be enabled and disabled by holding the HOME button for 2 seconds, and will function as a third joystick.
* The gyroscope can "aim-assist" the left or right analog sticks by holding down the L or R trigger while holding the HOME button to enable the gyroscope. Once enabled, hold the L or R trigger to have the gyroscope be applied to the left or right analog stick's input.
* The joysticks can be controlled by the d-pad by holding the HOME button and pressing in one of the joysticks for 2 seconds, for old 2D games that want to be controlled by a joystick.
* Simple force feedback is supported.
* The LED order indicator works for up to 8 unique controllers.

## Building & Installation
Run `make` to build using the makefile, then either load it temporarily with `make load` and `make unload`, or install it to load on the next boot with `make install` and `make uninstall`.

## Acknowledgement
Completion of this driver was aided significantly by dekuNukem's [Nintendo_Switch_Reverse_Engineering](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering) page, specifically CTCaer's rumble data and shinyquagsire23's UART command syntax.
