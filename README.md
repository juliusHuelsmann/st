
The `Alpha Focus Highlight` and the `Vim Browse` patches of the `suckless` `simple terminal` are developped in this repository.

`Vim Browse` adds history-functionality to the terminal, and allows to -- among other things -- select, yank, search it via keyboard using vim-like motions and operations. Branch: `plainVimPatchV1`.

The `Alpha Focus Highlight` patch applies transparency to the configured background, and allows to use different transparency levels and background colors for focused and unfocused windows. This patch requires a running X composite manager. Branch: `plainAlphaPatchV1`.

Both the `Vim Browse` and the `Alpha Focus Highlight` patches are applied to the `master` branch, together with a set of other patches (currently `xresources`, `externalPipe`) and adapted configuration files.

**Note** The benefit of the `alpha` patch and the `Alpha Focus Highlight` patch are the ability to restrict the transparency only to the background color currently in use, hence keeping the font in the foreground solid and readable.

If the goal is to apply transparency independent on the content, you do not require any patch for `st`, instead add
```bash
inactive-opacity = 0.2;
active-opacity = 0.8;
```
to your `picom` configuration file and keep a vanilla `st` build.

If you want to configure `inactive-opacity` and `active-opacity` rules in order to apply opacity to other applications, but keep the benefits of the st alpha aptches, have a look at [this picom configuration file](https://github.com/juliusHuelsmann/Config/blob/master/.config/picom/picom.conf), in which opacity management configured to be performed by `st`.

# Build and install process

## Build Dependencies
Build requirements: `make` `git` (optional).
Dependencies for alpha patch: e.g. `picom`.
```bash
sudo pacman -S make git picom
```

### 1. Clone

```bash
git clone https://github.com/juliusHuelsmann/st.git
cd st
```

### 2. Build

```bash
rm config.h
make clean
make
```

### 3. Install
On the `master`, this copies the shipped `.Xresources` file to the home directory. If the file already exists, the user is prompted and can opt to override.
```bash
sudo make install
```
In case you want to use the `.Xresources` (color scheme and opacity), 
be sure to copy  them into your home directory,
```bash
cp -i .Xresources $(HOME)/.Xresources
```
and to merge them after booting
```bash
xrdb -merge $(HOME)/.Xresources
```

## Apply Patch to clone of `st`
Depending on the version of `st` you are using as a basis, download a patch version from the [Release](https://github.com/juliusHuelsmann/st/releases) page, apply it via

```bash
cd [YOUR CLONE OF ST]
patch -p1 [FILENAME]
```
and build as described above.

From time to time, the patches will break against the `HEAD` of the `st` `master` branch. If you encounter patch conflicts when patching against the vanilla version of `st`, please report an issue. If you succeed to solve it yourself (or would like to propose some changes to the source code), your contribution is very welcome.


# Custom Patches:

## 1. Vim Browse Patch

Description
-----------

This patch offers the possibility to move through the terminal history, search for strings and use
VIM-like motions, operations and quantifiers.


Default Behavior:
-----------------
The default behavior listed below can be adapted:

**Vim Browse Mode**:
* Enter `Vim Browse Mode`: `Alt`+`c`
* leave `Vim Browse Mode`: `<esc>` (abort) or `<enter>` / `i` (accept)

**Operations in Vim Browse Mode**:
* Enter Visual Mode: `V` / `v`
* Enter Yank Mode: `Y`

**Motions in Vim Browse Mode**:
* Basic motions: `j`, `k,` `h`, `l`, `H`, `M`, `L`, `0`, `$` like in VIM
* Word Move operators: `w`, `W`, `e`, `E`, `b`, `B` similar to VIM
* Jump to the cursor position prior to entering Vim Browse Mode: `G`
* Repeat last command string: `.`
* forward / backward search: `n`, `N`
* `<ctrl>`+`u` resp. `<ctrl>`+`d` up / down half a page
* `<ctrl`>+ `b` resp. `<ctrl`>+ `f` like in vim (up / down a page, shifting current cursor position)
* ***Enter search Motion*** `/`, `?`,
   * Type search term.
* ***Inner Motion*** `i` (only valid during an operation)
   * Enables `yiw` / `yi{` / `yi[` / `...`  by substituting motions
* in Visual Mode `v`: use `t` to toggle block selection mode

**Quantifiers**
* Type a quantifier in order to execute a motion / operation multiple times.

**Custom Commands**:
Custom arrangements of the aforementioned commands can be defined in `config.h`:
```c++
struct NormalModeShortcuts normalModeShortcuts [] = {
	{ 'R', "?Building\n" },
	// ...
};
```
 Some use cases are:
- go to last executed command
- go to (first) compile error
- go to link

More information on the patch can be found at the [release page](https://github.com/juliusHuelsmann/st/releases).
The patch is developped at the branch [Version based on scrollback](https://github.com/juliusHuelsmann/st/tree/plainVimPatchV1).

Authors of the Vim-Browse Patch
--------------------------------
* Julius Hülsmann - <juliusHuelsmann [at] gmail [dot] com>
* [Kevin Velghe](https://github.com/paretje): Underline highlight fix


## 2. Alpha Selection Patch

Description
-----------
This patch allows the user to specify two distinct opacity values and background colors; one for the focused- and one for unfocused windows' background.
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

## Screenshot

![Screenshot](https://user-images.githubusercontent.com/9212314/68340852-7d6d9380-00e7-11ea-9705-51ed098eba2a.gif)


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

