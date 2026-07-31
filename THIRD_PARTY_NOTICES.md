# Third-party notices

The Windows release of Kalam includes the following third-party software:

- **Qt 6.8.3** — dynamically linked Qt Core, GUI, Network, and Widgets
  libraries and runtime plugins. Qt is available under the GNU Lesser General
  Public License v3 (and other licensing options). Corresponding Qt source is
  available from <https://download.qt.io/archive/qt/6.8/6.8.3/submodules/>.
- **Neovim 0.12.0** — licensed under the Apache License 2.0. Source and license:
  <https://github.com/neovim/neovim/tree/v0.12.0>.

Kalam's Neovim UI client (msgpack codec, RPC transport, grid renderer) is
first-party Kalam code; its keyboard-translation rules are an audited design
extraction from Neovim-Qt v0.2.19 (ISC License), rewritten in Kalam-owned
modules with no Neovim-Qt source distributed.

The portable archive includes copies of the applicable license texts in its
`licenses` directory. Kalam uses the Qt libraries dynamically; users may
replace the distributed Qt DLLs with compatible builds.
