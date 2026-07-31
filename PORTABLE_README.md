# Kalam Windows portable build

Double-click `run-kalam.bat` to open the bundled example scene. You can also
pass another scene from a terminal:

```bat
run-kalam.bat C:\path\to\scene.json
```

The archive includes the Qt runtime, Neovim, Kalam's Neovim runtime, and the
game module. No separate Qt or Neovim installation is required.

This binary archive is intended for running and evaluating the editor. Live
C/C++ module rebuilding still requires a Kalam source checkout plus the
project's compiler and Ninja build environment.
