#include "neovim_font_names.h"

#include <QFontDatabase>
#include <QPair>
#include <QStringList>
#include <QtDebug>

namespace Kalam::Editor
{

QString neovim_expand_gdi_font_abbreviation(const QString& requested)
{
    static const QPair<QString, QString> abbreviations[] = {
        { QStringLiteral(" NFM"), QStringLiteral(" Nerd Font Mono") },
        { QStringLiteral(" NFP"), QStringLiteral(" Nerd Font Propo") },
        { QStringLiteral(" NF"), QStringLiteral(" Nerd Font") },
    };
    for (const auto& abbreviation : abbreviations)
    {
        if (requested.endsWith(abbreviation.first, Qt::CaseInsensitive))
        {
            QString expanded = requested;
            expanded.chop(abbreviation.first.size());
            expanded += abbreviation.second;
            return expanded;
        }
    }
    return requested;
}

QString neovim_resolve_font_family(const QString& requested)
{
    if (requested.isEmpty())
    {
        return requested;
    }

    const QStringList available = QFontDatabase::families();
    for (const QString& family : available)
    {
        if (family.compare(requested, Qt::CaseInsensitive) == 0)
        {
            return family;
        }
    }

    const QString expanded = neovim_expand_gdi_font_abbreviation(requested);
    for (const QString& family : available)
    {
        if (family.compare(expanded, Qt::CaseInsensitive) == 0)
        {
            return family;
        }
    }

    /*
     * An empty database means no platform font plugin (the offscreen
     * platform in tests); that is not a misconfigured font, so stay quiet.
     */
    if (!available.isEmpty())
    {
        qWarning().noquote()
            << QStringLiteral("kalam: no installed font matches '%1'; Qt "
                              "will substitute a default")
                .arg(requested);
    }
    return requested;
}

} // namespace Kalam::Editor
