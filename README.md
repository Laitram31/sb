sb
==
Asynchronous modular status bar for dwm written in C

Notes
-----
- If you don't use [this build of dwm](https://github.com/ratakor/dwm) you will
need [this patch](https://gist.github.com/Ratakor/1a2ebc9a690dea31c19ac419e9e14138)
for the colors to work with dwm.

- sb can be configured on the fly with a config file located in
`$XDG_CONFIG_HOME/sb/config` (if XDG_CONFIG_HOME is not set,
`$HOME/.config/sb/config` will be used instead).

- It is possible to manually refresh a block with
`kill -sig $(pidof sb)`, see Configuration for the list of signals.
e.g. `kill -34 $(pidof sb)` will refresh the music block.

- Restart sb with `kill -10 $(pidof sb)`.

Installation
------------
Make sure to have these dependencies: libX11, libconfig, pthread and run

    # make install

After that you can put sb in your xinitrc or other startup script to have it
start with dwm.

Configuration
-------------
default configuration:
```
# Name       Active  Signal
music      = false   #  34
cputemp    = false   #  35
cpu        = false   #  36
memory     = false   #  37
battery    = false   #  38
wifi       = false   #  39
netspeed   = false   #  40
localip    = false   #  41
publicip   = false   #  42
volume     = false   #  43
mic        = false   #  44
news       = false   #  45
weather    = false   #  46
daypercent = false   #  47
date       = false   #  48
time       = true    #  49
```
