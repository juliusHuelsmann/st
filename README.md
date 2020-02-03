# Custom Patches:

## 1. Vim Browse Patch

![Screenshot](https://user-images.githubusercontent.com/9212314/68340852-7d6d9380-00e7-11ea-9705-51ed098eba2a.gif)


Description
-----------

This patch offers the possibility to move through the terminal history, search for strings and use
VIM-like motions, operations and quantifiers.


Default Behavior:
-----------------
The default behavior listed below can be adapted:

**Enter Vim Browse Mode**:
* `Alt`+`c`

**Operations in Vim Browse Mode**:
* Enter Visual Mode: `V` / `v`
* Enter Yank Mode: `Y`

**Motions in Vim Browse Mode**:
* Basic motions: `j`, `k,` `h`, `l`, `H`, `M`, `L`, `0`, `$` like in VIM
* Word Move operators: `w`, `W`, `e`, `E`, `b`, `B` similar to VIM
* Search Operators: `/`, `?`, `n`, `N` for forward / backward search
* Jump to the cursor position prior to entering Vim Browse Mode: `G`
* Repeat last command string: `.`
* in Visual Mode `v`: use `t` to toggle block selection mode

**Custom Commands**:
Custom arrangements of the aforementioned commands can be defined in the configuration file.

##### Use cases
- [Use Case: custom command: jump to first error when compiling from out of vim](https://user-images.githubusercontent.com/9212314/71112829-aa27c700-21cc-11ea-9eae-390b9760e4ca.gif)
    - using shortcut `S` defined in configuration file

More information on the patch can be found at the [release page](https://github.com/juliusHuelsmann/st/releases).

The patch is developped at the branch [Version based on scrollback](https://github.com/juliusHuelsmann/st/tree/plainVimPatchV1).


Authors of the Vim-Browse Patch
--------------------------------
* Julius Hülsmann - <juliusHuelsmann [at] gmail [dot] com>
* [Kevin Velghe](https://github.com/paretje): Underline highlight fix




## 2. Alpha Selection Patch

![Screenshot](https://user-images.githubusercontent.com/9212314/68339985-e48a4880-00e5-11ea-8ff3-4e7086ad93c0.gif)

Description
-----------
This patch allows the user to specify two distinct opacity values; one for the focused- and one for
unfocused windows' background.
This enables the user to spot the focused window at a glance.

The *Alpha Highlight Focus Patch* patch is based on the
[Alpha Patch](https://st.suckless.org/patches/alpha/),
which is already applied in the patch file below. Most of the work has been performed by the
original authors of the Alpha Patch.

More information on the patch can be found at the [release page](https://github.com/juliusHuelsmann/st/releases).

The patch is developped at [this  branch](https://github.com/juliusHuelsmann/st/tree/plainAlphaPatchV1).


Authors of the Alpha-Focus Patch
--------------------------------
* Julius Hülsmann - <juliusHuelsmann [at] gmail [dot] com>
* [glpub](https://github.com/glpub): Fix: erroneous color reset  
* [Milos Stojanovic](https://github.com/M4444): Code Formatting




















## St 
st - simple terminal
--------------------
st is a simple terminal emulator for X which sucks less.


Requirements
------------
In order to build st you need the Xlib header files.


Installation
------------
Edit config.mk to match your local setup (st is installed into
the /usr/local namespace by default).

Afterwards enter the following command to build and install st (if
necessary as root):

    make clean install


Running st
----------
If you did not install st with make clean install, you must compile
the st terminfo entry with the following command:

    tic -sx st.info

See the man page for additional details.

Credits
-------
Based on Aurélien APTEL <aurelien dot aptel at gmail dot com> bt source code.

