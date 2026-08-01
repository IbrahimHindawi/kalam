#ifndef KALAM_EDITOR_QT_NEOVIM_FONT_NAMES_H
#define KALAM_EDITOR_QT_NEOVIM_FONT_NAMES_H

#include <QString>

namespace Kalam::Editor
{

/*
 * Font-family naming differs between the world Neovim's 'guifont' comes
 * from and the world Qt matches in.
 *
 * Terminals, GDI, and therefore the font names users copy into 'guifont'
 * use GDI family names, which are capped at 31 characters; Nerd Font
 * patchers abbreviate "Nerd Font" to "NF" to fit. Qt matches DirectWrite
 * typographic names, which spell it out. So a perfectly valid
 * "IosevkaTerm NF" finds nothing in Qt, and Qt answers an unmatched
 * family by silently substituting a default -- the wrong font appears
 * with no error anywhere.
 *
 * Every place that turns a 'guifont' string into a QFont must resolve
 * through here, so the grid and the Qt surfaces can never disagree.
 */

/* Pure suffix mapping: " NF" -> " Nerd Font", and the Mono/Propo forms. */
QString neovim_expand_gdi_font_abbreviation(const QString& requested);

/*
 * The installed family matching requested: an exact match first, then the
 * expanded abbreviation. Returns requested unchanged (and warns) when
 * nothing matches, so the caller still gets Qt's substitution rather than
 * an empty family.
 */
QString neovim_resolve_font_family(const QString& requested);

} // namespace Kalam::Editor

#endif
