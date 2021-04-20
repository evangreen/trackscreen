# Trackscreen: Make a trackpad out of a touchscreen

Trackscreen is a silly utility that emulates a trackpad (mouse) device based on touchscreen inputs. It grabs exclusive control of the touchscreen, and responds to touches that begin in the bottom center square of the touchscreen (imagine the touchscreen divided up into a 3x3 grid). It converts those absolute touch events into relative touch events, that are passed on through the new emulated trackpad. Short taps are passed on as left button clicks.

trackscreen works on Linux devices. It depends on a few Linux headers and libudev.

Usage: `bin/trackscreen [-s scale] /dev/input/eventXY`

Use evtest to figure out which device to pass along the command line. If evtest is showing you reports like ABS_MT_POSITION_X, then you've probably got the right device. You can set something like -s 0.5 to make the mouse respond less wildly, or -s 2.0 to make the cursor extremely
zippy.
