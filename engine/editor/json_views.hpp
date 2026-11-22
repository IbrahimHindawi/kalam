#pragma once

#include <QByteArray>
#include <QHash>
#include <QPointer>
#include <QStringList>
#include <QVariantList>
#include <functional>

class QDialog;
class QLabel;
class QWidget;
namespace Kalam::Editor {
class Neovim_Grid_Widget;
class Neovim_Rpc_Client;
}

class JsonViews {
public:
    using Validate = std::function<QString(const QString &, const QByteArray &)>;
    using Preview = std::function<void(const QString &, const QByteArray &)>;
    JsonViews(Kalam::Editor::Neovim_Grid_Widget *grid,
              Kalam::Editor::Neovim_Rpc_Client *rpc, Validate validate, Preview preview, QString scenePath,
              bool useNeovimPaneKeys);
    ~JsonViews();
    bool notify(const QByteArray &name, const QVariantList &args);
    void layout();
    void resetPreview();

    struct Document {
        qint64 window, buffer, revision;
        QString path;
        QByteArray text;
        QByteArray panel;
    };

private:
    Kalam::Editor::Neovim_Grid_Widget *grid_;
    Kalam::Editor::Neovim_Rpc_Client *rpc_;
    Validate validate_;
    Preview preview_;
    QString scenePath_;
    bool useNeovimPaneKeys_;
    qint64 draftWindow_ = 0;
    qint64 draftBuffer_ = 0, draftRevision_ = 0;
    qint64 sourceBuffer_ = 0, sourceRevision_ = 0;
    qint64 appliedBuffer_ = 0;
    float appliedExposure_ = 0;
    QHash<qint64, Document> documents_;
    QHash<qint64, QWidget *> panes_;
    struct FormView {
        qint64 buffer;
        QStringList selection;
        int scroll;
    };
    QHash<qint64, FormView> formViews_;
    QPointer<QDialog> dialog_;
    bool forms_ = false;
    bool cmdline_ = false;
    qint64 documentDraftWindow_ = 0;

    void field(const Document &document, qsizetype offset, bool exact = false,
               const QByteArray &controlSpec = {});
    void editExposure(const Document &document, qsizetype start, qsizetype end,
                      double value, bool apply, QDialog *dialog = nullptr, QLabel *error = nullptr,
                      bool tui = false);
    void acceptExposure(const Document &doc, double value);
    void cancelPreview(qint64 window);
    void previewValue(const Document &document, qsizetype start, qsizetype end, const QByteArray &value);
    void tuiApplied(qint64 window, const QString &message = {});
    QWidget *makePane(const Document &document);
    void rememberForm(qint64 window, QWidget *pane);
    void commit(const Document &document, qsizetype start, qsizetype end,
                const QByteArray &replacement, QDialog *dialog, QLabel *error,
                std::function<void()> rejected = {}, std::function<void()> accepted = {},
                std::function<void(const QString &)> report = {});
};
