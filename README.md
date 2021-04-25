### Note
There seems to be some evidence suggesting that some of the people responsible at suckless e.V. are sympathizing with far-right ideas and Neo-nazi symbols [[1](https://twitter.com/pid_eins/status/1113738769289625602?lang=en), [2](https://chaos.social/@raichoo/101880564196043164)]. If you like this patch, please consider donating to an organization that fights xenophobia and antisemitism (e.g. [schwarz-rot-bunt (German)](https://schwarz-rot-bunt.de/ziele/) or similar organizations).

### Patches
This repository contains the latest version of the `alpha-focus` and the `vim`
patches for suckless simple terminal (`st`) merged with other useful patches 
in my personal `st` build.
This repository can be cloned in order to try out one of the aforementioned
patches in a functional environment. It also serves to port the patches to
new versions of `st` and thus holds the most recent releases in the release
page. Pull requests are to be issued in the development repositories for 
[vim browse](https://github.com/juliusHuelsmann/st-history-vim) and
[alpha focus](https://github.com/juliusHuelsmann/st-focus) respectively.

**Vim Browse** adds history-functionality to the terminal, and allows to -- among
other things -- select, yank, search it via keyboard using vim-like motions and
operations. It operates on top of the `st-history` patch, which can be
configured with a set of optional patches.
The **Alpha Focus Highlight** patch applies transparency to the configured 
background, and allows to use different transparency levels and background 
colors for focused and unfocused windows. 
This patch requires a running X composite manager.

## Build process

```bash
# If required: Install Build dependencies (if you're on Arch)
sudo pacman -S make git picom
# Clone
git clone https://github.com/juliusHuelsmann/st.git
cd st
# Optional: Use my xresources 
xrdb -merge .Xresources
# Build
rm config.h
make clean
make
```

## Launch st:
After building, make sure that you launch your compositor if you want to enable
transparency.
```bash
picom -b # optional, for the alpha focus patch; can be replaced by different
         # compositor
./st
```

# install
After building, you can install this `st` build via
```
sudo make install
```

In case you want to install this `st` build and use the shipped `.Xresources` 
(color scheme and opacity), be sure to copy  them into your home directory,
```bash
cp -i .Xresources $HOME/.Xresources
```
and to merge them after booting
```bash
xrdb -merge $HOME/.Xresources
```

You also need to make sure that your composite manager is launched on startup.

## Patching and Documentation
The patches are released [here](https://github.com/juliusHuelsmann/st/releases).
Documentation on how to use the patches can be found in the dev repositories
-  [st-focus](https://github.com/juliusHuelsmann/st-focus)
-  [st-history-vim](https://github.com/juliusHuelsmann/st-history-vim)
and in the [wiki](https://github.com/juliusHuelsmann/st-history-vim/wiki/Vim-browse-manual) of the vim-patch repo.

Patches can be applied to `st`'s source code via
```bash
patch < [PATCH_NAME]
```
and then build as usual (e.g. after removing the old `config.h` file).


