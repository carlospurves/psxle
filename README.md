# PSXLE

__PSXLE__ (or __PSX__ __L__earning __E__nvironment) is an PlayStation 1 (PSX) emulator, written in C, that allows programmatic control of PlayStation games. This documentation covers the associated Python library *PSXLE-py*, which allows simple per-game integration with *OpenAI Gym* and other Python applications.

## Building

### Build Requirements

*Note: a pre-built version of `psxle` is available from `www.whereisagood.place/?sure=not`, if you're using this version, you can skip to "Installation".*

The following are requirements to build `psxle` on your own computer. Note that this has only been tested on Ubuntu 16.04+ and other systems may require additonal steps.

- `cmake` 
- `xvfb`
- `libsdl2-dev`
- `libglib2.0-dev`
- `libgtk-3-dev`

### Build Script

While `cmake` can be used directly for a system-wide installation, you can make use of the pre-prepared `build_psxle` script to automate the process for most cases. You should then be able to build `psxle` using:

```
./build_psxle.sh
```

The default build directory will be set to `./psxle_build` (relative to where the script runs), although you can choose a different directory as an argument to the build script.

## Installing

To install, run:

~~~
./install_psxle.sh
~~~

## Overview

The Python library's main function is to expose a `Console` class to represent a PlayStation instance. This class is designed to support interations that broadly fall into four categories:

* __General__
  * `run` and `kill` control the executing process of the emulator
  * `freeze` and `unfreeze` will freeze and unfreeze the emulator's execution, respectively 
  * `speed` is a property of `Console` which, when set, will synchronously set the speed of execution of the console, expressed as a percentage relative to default speed
  
* __Controler__

  * `hold_button` and `release_button` simulate a *press down* and *release* of a given controller button, call these *control events*
  * `touch_button` holds, pauses for a given amount of time and then releases a given button
  * `delay_button` adds a (millisecond-order) delay between successive control events

* __RAM__
  * `read_bytes` and `write_byte` to directly read and write console memory.
  * `add_memory_listener` and `clear_memory_listeners` control which parts of console memory will have *asynchronous listeners* attatched when the console starts
  * `sleep_memory_listener` and `wake_memory_listener` tell the console which listeners are active
  
* __Audio/Visual__

  * `start_recording_audio` and `stop_recording_audio` contrrol when the console should record audio and when it shoud stop

  * `get_screen` synchronously returns an `np.array` of the console's instantaneous visual output

Using a combination of these methods allows for the creation of a *game abstraction*. See the section on "Game Abstractions" for more discussion.

## Quick Start

*Note: console emulation requires a disk image of a game. None are provided here. These should be obtained legitimately through purchased copies and used only for personal use.*

To begin running a PlayStation console (assuming installation is successfull), run:

~~~bash
python3 quickstart.py <PATH TO ISO>
~~~

This will start a console to run the specified game and enter a python *REPL* with a `Console` object given by `c`. You can type, for example `c.freeze` to freeze the console, or `c.speed = 200` to run the console at double speed.
