# TODO !
- [ ] Fluorite 0.5
    - [ ] Better popup functions
    - [x] Add a way to kill popup windows with keybinds when focused
    - [ ] Rewrite the redraw tiling function, needs many many refactoring on maths. It's really ugly even if it works fine
    - [ ] Find why some apps make fullscreen bugs when closing (feh for example)

# PARTIALLY DONE
- [-] Style the slaves stacking design
    - [ ] Add a right Master option
    - [ ] Add an option to do top-right to bottom-left, insteam of top-left to bottom-right (?) 
- [-] Fix the issue where windows are not resized to the frame size
    - [ ] Look for the XConfigure and the reparenting
    - [ ] Look for hints/atoms/attributes
    - [x] Try hacky fixes with XMove and XResize windows before reparenting. DWM seems to do a XMoveResize even if not needed (IF ALL ELSE FAILS) 
        - Seems to be the only solution right now, but it causes some kind of jumping on master switching
        - Firefox still stuggle sometimes (mainly at the opening)
- [-] Fix the custom workspace names
    - Works but doesn't support nerdfont and longer names properly

# Not Really Important
- [ ] Fix the popup window not showing as popup (something with atoms or hints ? i think ragnar does it nicely)
- [ ] Some very precise apps, make Fluorite Crash
- [ ] killall command can sometimes break stuff (need more testing)

# DONE !
- [x] Fixing the multi closing issue
    - Fixed with XSync(fluorite.display, True) and X(Un)GrabServer
- [x] Add and manage the config file
- [x] Add rotations to slaves stack
- [x] Add master window resize
- [x] Bind global keys
    - Needs to define the keyboard inside xinitrc with `setxkbmap -layout fr`
- [x] Fixing the not full focus issue. Probably something to do with atoms, hints, or window config
    - Might be a partial fix: XSetInputFocus on `master_winframe->window`
- [x] Add transparent windows (XCreateWindows and not XCreateSimpleWindows)
    - Seems to be working, Might be wrong in some cases
    - Seems like picom can't affect it and should be done inside program config
    - Maybe try to find a way to do it inside config file
- [x] Fixed issues with dmenu, rofi, firefox
    - Was just a bad variable management inside the unmap function
    - Added a closed variable and moving `fluorite.frames_count` inside the if statement fixed it
- [x] Add Workspaces
- [x] Add Fullscreen mode
- [x] Fix the closing windows outside the current workspaces
    - Should be fixed, stay vigilent for potential bugs after the whole rewrite of unmapping and changes made to organise stack
- [x] Check the binding for the "à" in XK_, it seems to not be working
- [x] Better closing function, avoid issues with Firefox for example
- [x] Keyboard focus on first slaves if mouse enter
    - Still apply Fluorite bindings based on the master window
- [x] Opening two thunar and closing one, makes the window manager dies
    - Fixed itself for some reason
- [x] Killing a firefox instance, kills all Firefox instances
    - Fixed by itself ?
- [x] Clean the SAME_BORDER code, it's now useless
- [x] Remove FluoriteBar code since it's not usefull any more (just use polybar !)
- [x] Add a force new master focus to the EnterNotify (REALLY NEEDED)
    - Might need some more testing
- [x] Fix writing of wifi module when not connected
- [x] Create a make install
- [x] Closing binding now have effect on the focused window
    - Might cause some issues
- [x] Moving window to another workspace now have effect on the focused window
    - Might cause some issues
- [x] Resize Window go to negative value
- [x] New binding to restore default master offset
- [x] Adding a default master offset value in config file
- [x] Find a way to only have one border when stacked
