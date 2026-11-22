#include "embedded.h"
#include "entity_inspector.h"
#include "json_views.hpp"
#include "neovim_font_names.h"
#include "neovim_grid_widget.h"
#include "neovim_rpc_client.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QSlider>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTreeWidget>
#include <QPushButton>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFocusEvent>
#include <QDir>
#include <QJsonDocument>
#include <QPointer>
#include <QMessageBox>
#include <QMainWindow>
#include <QRegion>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QKeyEvent>
#include <QTimer>
#include <functional>
#include <cstdio>
#include <cmath>
#include <array>
#include <memory>
#include <limits>

using namespace Kalam::Editor;

namespace {
QString validateSceneDocuments(const QByteArray &scene, const QJsonObject &overrides = {}) {
    const auto bytes = QJsonDocument(overrides).toJson(QJsonDocument::Compact);
    char message[1024] = {};
    if (kalam_validate_documents(scene.constData(), bytes.constData(), size_t(bytes.size()),
                                 message, sizeof(message)))
        return {};
    return QString::fromUtf8(message);
}

void installEditorTheme(QApplication &app) {
    QApplication::setStyle("Fusion");
    QPalette palette = app.palette();
    palette.setColor(QPalette::Window, QColor("#303236"));
    palette.setColor(QPalette::Base, QColor("#242529"));
    palette.setColor(QPalette::AlternateBase, QColor("#292b2f"));
    palette.setColor(QPalette::Button, QColor("#3a3c40"));
    palette.setColor(QPalette::WindowText, QColor("#e3dfd8"));
    palette.setColor(QPalette::Text, QColor("#e3dfd8"));
    palette.setColor(QPalette::ButtonText, QColor("#e3dfd8"));
    palette.setColor(QPalette::PlaceholderText, QColor("#a39d94"));
    palette.setColor(QPalette::Accent, QColor("#dfa26c"));
    palette.setColor(QPalette::Highlight, QColor("#554233"));
    palette.setColor(QPalette::HighlightedText, QColor("#fff0da"));
    palette.setColor(QPalette::ToolTipBase, QColor("#35373b"));
    palette.setColor(QPalette::ToolTipText, QColor("#e3dfd8"));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, QColor("#8e8a84"));
    app.setPalette(palette);

    QFile sheet(":/kalam/editor.qss");
    if (sheet.open(QIODevice::ReadOnly))
        app.setStyleSheet(QString::fromUtf8(sheet.readAll()));
}
}

class Viewport final : public QWidget {
public:
    Viewport(QWidget *parent, std::function<void()> splitPrefix, std::function<void()> select)
        : QWidget(parent), splitPrefix_(std::move(splitPrefix)), select_(std::move(select)) {}

protected:
    void keyPressEvent(QKeyEvent *event) override {
        if (splitPending_ && event->key() != Qt::Key_Control && event->key() != Qt::Key_Shift &&
            event->key() != Qt::Key_Alt && event->key() != Qt::Key_Meta) {
            const bool count = event->key() >= Qt::Key_0 && event->key() <= Qt::Key_9 &&
                event->modifiers() == Qt::NoModifier;
            const bool subcommand = event->key() == Qt::Key_G &&
                event->modifiers() == Qt::NoModifier && !splitSubcommand_;
            splitPending_ = count || subcommand;
            splitSubcommand_ = subcommand;
            if (event->key() == Qt::Key_Escape) kalam_embed_clear_pick();
            QApplication::sendEvent(parentWidget(), event);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            kalam_embed_clear_pick();
            event->accept();
        } else if (event->key() == Qt::Key_W && event->modifiers() == Qt::ControlModifier) {
            if (!event->isAutoRepeat()) {
                keys_ = 0;
                kalam_embed_camera_keys(0);
                splitPending_ = true;
                splitSubcommand_ = false;
                splitPrefix_();
            }
            event->accept();
        } else if (event->text() == ":") {
            parentWidget()->setFocus();
            QApplication::sendEvent(parentWidget(), event);
            event->accept();
        } else if (event->key() == Qt::Key_Space) {
            if (!event->isAutoRepeat()) kalam_embed_pick();
            event->accept();
        } else if (const auto key = cameraKey(event->key())) {
            keys_ |= key;
            kalam_embed_camera_keys(keys_);
            event->accept();
        } else {
            QWidget::keyPressEvent(event);
        }
    }

    void keyReleaseEvent(QKeyEvent *event) override {
        if (const auto key = cameraKey(event->key())) {
            if (!event->isAutoRepeat()) {
                keys_ &= ~key;
                kalam_embed_camera_keys(keys_);
            }
            event->accept();
        } else {
            QWidget::keyReleaseEvent(event);
        }
    }

    void focusOutEvent(QFocusEvent *event) override {
        keys_ = 0;
        if (splitPending_) {
            QKeyEvent cancel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(parentWidget(), &cancel);
        }
        splitPending_ = false;
        splitSubcommand_ = false;
        kalam_embed_camera_keys(0);
        QWidget::focusOutEvent(event);
    }

    void focusInEvent(QFocusEvent *event) override {
        QWidget::focusInEvent(event);
        if (event->reason() == Qt::MouseFocusReason) select_();
    }

private:
    std::function<void()> splitPrefix_, select_;
    unsigned keys_ = 0;
    bool splitPending_ = false, splitSubcommand_ = false;
    static unsigned cameraKey(int key) {
        switch (key) {
        case Qt::Key_H: return KALAM_CAMERA_H;
        case Qt::Key_J: return KALAM_CAMERA_J;
        case Qt::Key_K: return KALAM_CAMERA_K;
        case Qt::Key_L: return KALAM_CAMERA_L;
        case Qt::Key_O: return KALAM_CAMERA_O;
        case Qt::Key_M: return KALAM_CAMERA_M;
        case Qt::Key_A: return KALAM_CAMERA_A;
        case Qt::Key_S: return KALAM_CAMERA_S;
        case Qt::Key_D: return KALAM_CAMERA_D;
        case Qt::Key_F: return KALAM_CAMERA_F;
        default: return 0;
        }
    }
};

class Editor final : public QMainWindow {
public:
    Editor(QString scene, QString root, bool smoke, bool gui)
        : scene_(scene.toUtf8()), root_(root), source_(root + "/scenes/" + scene + ".json"),
          smoke_(smoke), gui_(gui) {
        setWindowTitle("Kalam");
        resize(1100, 680);
        rpc_ = new Neovim_Rpc_Client(this);
        grid_ = new Neovim_Grid_Widget(rpc_, this);
        if (!gui_) setCentralWidget(grid_);
        json_ = std::make_unique<JsonViews>(grid_, rpc_, [this](const QString &path, const QByteArray &bytes) {
            const auto relative = livePath(path);
            if (relative.isEmpty()) return QString{};
            auto candidate = liveDocuments_;
            candidate.insert(relative, QString::fromUtf8(bytes));
            return validateSceneDocuments(scene_, candidate);
        }, [this](const QString &path, const QByteArray &bytes) {
            const auto relative = livePath(path);
            if (!path.isEmpty() && relative.isEmpty()) return;
            if (relative.isEmpty() && draftPath_.isEmpty()) return;
            draftPath_ = relative;
            draftBytes_ = bytes;
            previewTimer_.start();
        }, source_, !gui_);
        connect(grid_, &Neovim_Grid_Widget::guifontChanged, this, [this](const QString &option) {
            const auto parts = option.section(',', 0, 0).split(':');
            if (parts.isEmpty() || parts.front().isEmpty())
                return;
            QFont font = grid_->grid_font();
            QString family = parts.front();
            family.replace(QStringLiteral("\\ "), QStringLiteral(" "));
            font.setFamily(neovim_resolve_font_family(family));
            for (const auto &part : parts.sliced(1)) {
                bool valid = false;
                const auto height = part.startsWith('h') ? part.mid(1).toDouble(&valid) : 0;
                if (valid && height >= 6 && height <= 64)
                    font.setPointSizeF(height);
            }
            grid_->set_grid_font(font);
            layoutViewport();
            json_->layout();
        });
        debounce_.setSingleShot(true);
        debounce_.setInterval(150);
        previewTimer_.setSingleShot(true);
        previewTimer_.setInterval(75);
        connect(&previewTimer_, &QTimer::timeout, this, [this] { liveReload_ = true; });
        connect(&debounce_, &QTimer::timeout, this, [this] {
            watchContent();
            if (gui_) refreshSceneList();
            reload_ = true;
            if (rpc_->is_ready())
                rpc_->call("nvim_command", {"checktime"});
        });
        connect(&watcher_, &QFileSystemWatcher::fileChanged, &debounce_, qOverload<>(&QTimer::start));
        connect(&watcher_, &QFileSystemWatcher::directoryChanged, &debounce_, qOverload<>(&QTimer::start));
        watchContent();
        viewport_ = new Viewport(gui_ ? static_cast<QWidget *>(this) : grid_,
                                 [this] { rpc_->notify("nvim_input", {"<C-w>"}); }, [this] {
            if (viewportWindow_) rpc_->notify("nvim_set_current_win", {viewportWindow_});
        });
        viewport_->setAttribute(Qt::WA_NativeWindow);
        viewport_->setAttribute(Qt::WA_NoSystemBackground);
        viewport_->setFocusPolicy(Qt::ClickFocus);
        viewport_->hide();
        if (gui_) setupGuiWorkspace();
        connect(grid_, &Neovim_Grid_Widget::gridLayoutChanged, this, [this] {
            layoutViewport();
            json_->layout();
        });
        connect(rpc_, &Neovim_Rpc_Client::ready, this, [this] { grid_->attach_ui(); });
        connect(grid_, &Neovim_Grid_Widget::attached, this, [this] { initNeovim(); });
        connect(rpc_, &Neovim_Rpc_Client::notificationReceived, this,
            [this](const QByteArray &name, const QVariantList &args) {
                if (json_->notify(name, args))
                    return;
                if (gui_ && name == "kalam.focus_viewport") {
                    focusDock(viewportDock_);
                } else if (name == "kalam.viewport" && args.size() == 2 && !gui_) {
                    const auto window = args[0].toLongLong();
                    const bool current = window && args[1].toLongLong() == window;
                    if (current && (!viewportCurrent_ || viewportWindow_ != window))
                        viewportFocusPending_ = true;
                    if (!current) {
                        viewportFocusPending_ = false;
                        if (viewport_->hasFocus()) grid_->setFocus();
                    }
                    viewportWindow_ = window;
                    viewportCurrent_ = current;
                    layoutViewport();
                } else if (name == "kalam.viewport_command_done" && viewportCurrent_) {
                    viewportFocusPending_ = true;
                    layoutViewport();
                } else if (name == "kalam.live_documents" && args.size() == 1) {
                    QJsonObject documents;
                    const auto absolute = QJsonDocument::fromJson(args[0].toString().toUtf8()).object();
                    for (auto it = absolute.begin(); it != absolute.end(); ++it) {
                        const auto relative = livePath(it.key());
                        if (!relative.isEmpty() && it.value().isString()) documents.insert(relative, it.value());
                    }
                    if (documents != liveDocuments_) {
                        liveDocuments_ = std::move(documents);
                        previewTimer_.start();
                    }
                } else if (name == "kalam.reload") {
                    debounce_.start();
                } else if (name == "kalam.inspect" && args.size() == 4) {
                    inspect(args[0].toLongLong(), args[1].toLongLong(),
                            args[2].toString().toUtf8(), args[3].toString().toUtf8());
                }
            });
        connect(rpc_, &Neovim_Rpc_Client::processExited, this, [this](int status) {
            if (gui_ && !closing_ && !smoke_) {
                documentDock_->hide();
                statusBar()->showMessage("Neovim closed. The viewport is still running.");
                return;
            }
            stop_ = true;
            result_ = status;
            if (!running_)
                QApplication::exit(result_);
        });
        QTimer::singleShot(0, this, [this] {
            const QStringList opts = smoke_ ? QStringList{"-u", "NONE", "-i", "NONE"} : QStringList{};
            if (rpc_->spawn("nvim", opts)) return;
            if (gui_ && !smoke_) {
                documentDock_->hide();
                statusBar()->showMessage("Put Neovim on PATH to edit documents.");
            } else {
                fail("Neovim startup failed. Check that nvim is on PATH.");
            }
        });
        if (gui_) scheduleViewport();
        if (smoke_)
            QTimer::singleShot(15000, this, [this] {
                std::fprintf(stderr, "Editor smoke stage %d, grid font %.1f, Qt font %.1f\n",
                             stage_, grid_->grid_font().pointSizeF(), QApplication::font().pointSizeF());
                std::fprintf(stderr, "Last drawn exposure %.3f\n", kalam_embed_drawn_exposure());
                fail("Viewport smoke timed out");
            });
    }

protected:
    void closeEvent(QCloseEvent *event) override {
        if (rpc_->is_ready() && !stop_) {
            event->ignore();
            closing_ = true;
            auto *request = rpc_->call("nvim_command", {"confirm qa"});
            connect(request, &Neovim_Rpc_Request::finished, this, [this] { closing_ = false; });
            connect(request, &Neovim_Rpc_Request::failed, this, [this] { closing_ = false; });
        } else {
            stop_ = true;
            event->accept();
        }
    }

    bool eventFilter(QObject *object, QEvent *event) override {
        if (gui_ && (object == viewport_ || object == sceneList_) && event->type() == QEvent::KeyPress &&
                static_cast<QKeyEvent *>(event)->text() == ":") {
            command_->setFocus();
            return true;
        }
        return QMainWindow::eventFilter(object, event);
    }

private:
    Neovim_Rpc_Client *rpc_;
    Neovim_Grid_Widget *grid_;
    QWidget *viewport_;
    QDockWidget *viewportDock_ = nullptr, *documentDock_ = nullptr, *sceneDock_ = nullptr;
    QListWidget *sceneList_ = nullptr;
    QLineEdit *command_ = nullptr;
    QByteArray scene_;
    QString root_;
    QString source_;
    QFileSystemWatcher watcher_;
    QTimer debounce_;
    QTimer previewTimer_;
    QJsonObject liveDocuments_;
    QString draftPath_;
    QByteArray draftBytes_;
    bool liveReload_ = false;
    QPointer<EntityInspector> inspector_;
    std::unique_ptr<JsonViews> json_;
    qint64 viewportWindow_ = 0;
    bool viewportCurrent_ = false, viewportFocusPending_ = false;
    bool smoke_, running_ = false, scheduled_ = false, stop_ = false, reload_ = false;
    bool gui_, closing_ = false, guiDocumentReady_ = false;
    int result_ = 0, stage_ = 0;
    QRect initial_;
    unsigned hiddenFrames_ = 0;
    unsigned initialDraws_ = 0;
    QByteArray originalEntity_;

    QString scenePath() const { return "scenes/" + QString::fromUtf8(scene_) + ".json"; }
    QString entityDirectory() const { return "scenes/" + QString::fromUtf8(scene_); }
    QString entityPath(const QString &id) const { return entityDirectory() + "/" + id + ".json"; }
    QString livePath(const QString &path) const {
        if (path.isEmpty()) return {};
        const auto relative = QDir(root_).relativeFilePath(QFileInfo(path).absoluteFilePath());
        if (relative == scenePath() || relative == "materials/reference.json") return relative;
        const auto prefix = entityDirectory() + "/";
        if (relative.startsWith(prefix) && relative.endsWith(".json") &&
                !relative.sliced(prefix.size()).contains('/')) return relative;
        return {};
    }

    void setupGuiWorkspace() {
        setDockOptions(AllowNestedDocks | AllowTabbedDocks | GroupedDragging);
        auto *view = menuBar()->addMenu("View");
        const auto dock = [this, view](const QString &title, const QString &name, QWidget *widget,
                                      Qt::DockWidgetArea area) {
            auto *pane = new QDockWidget(title, this);
            pane->setObjectName(name);
            pane->setWidget(widget);
            addDockWidget(area, pane);
            view->addAction(pane->toggleViewAction());
            return pane;
        };
        sceneList_ = new QListWidget(this);
        sceneList_->setMinimumWidth(155);
        sceneDock_ = dock("Scene", "kalam-scene-dock", sceneList_, Qt::LeftDockWidgetArea);
        viewportDock_ = dock("Viewport", "kalam-viewport-dock", viewport_, Qt::RightDockWidgetArea);
        documentDock_ = dock("Document", "kalam-document-dock", grid_, Qt::LeftDockWidgetArea);
        splitDockWidget(sceneDock_, documentDock_, Qt::Vertical);
        viewport_->show();
        resizeDocks({sceneDock_, viewportDock_}, {410, 690}, Qt::Horizontal);
        resizeDocks({sceneDock_, documentDock_}, {145, 470}, Qt::Vertical);
        refreshSceneList();
        connect(sceneList_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
            if (!item) return;
            const auto id = item->data(Qt::UserRole).toString().toUtf8();
            kalam_select_entity(id.constData());
            openGuiDocument(QString::fromUtf8(id));
        });
        connect(sceneList_, &QListWidget::itemActivated, this, [this] { focusDock(documentDock_); });
        connect(viewportDock_, &QDockWidget::visibilityChanged, this, [this](bool visible) {
            if (visible) scheduleViewport();
        });
        auto *document = menuBar()->addMenu("Document");
        document->addAction("Form", this, [this] { runGuiCommand("form"); });
        document->addAction("JSON / Text", this, [this] { runGuiCommand("text"); });
        document->addAction("Components", this, [this] { runGuiCommand("entities"); });
        document->addAction("Save All", this, [this] { runGuiCommand("write"); });
        command_ = new QLineEdit(this);
        command_->setAccessibleName("Kalam command");
        command_->setPlaceholderText(":  viewport, scene, form, text, write");
        command_->setCompleter(new QCompleter(
            QStringList{"viewport", "scene", "form", "text", "entities", "write"}, command_));
        statusBar()->addPermanentWidget(command_, 1);
        connect(command_, &QLineEdit::returnPressed, this, [this] {
            const auto command = command_->text().trimmed();
            command_->clear();
            runGuiCommand(command);
        });
        auto *commandShortcut = new QShortcut(QKeySequence("Ctrl+Shift+P"), this);
        commandShortcut->setContext(Qt::ApplicationShortcut);
        connect(commandShortcut, &QShortcut::activated, command_, qOverload<>(&QWidget::setFocus));
        for (const auto [key, direction] : std::array{
                 std::pair{'H', QPoint(-1, 0)}, std::pair{'J', QPoint(0, 1)},
                 std::pair{'K', QPoint(0, -1)}, std::pair{'L', QPoint(1, 0)}}) {
            auto *shortcut = new QShortcut(QKeySequence(QString("Ctrl+W, %1").arg(QChar(key))), this);
            shortcut->setContext(Qt::ApplicationShortcut);
            connect(shortcut, &QShortcut::activated, this, [this, direction] { focusGuiNeighbor(direction); });
        }
        viewport_->installEventFilter(this);
        sceneList_->installEventFilter(this);
    }

    void refreshSceneList() {
        QFile file(source_);
        if (!file.open(QIODevice::ReadOnly)) return;
        const auto entities = QJsonDocument::fromJson(file.read(256 * 1024 + 1)).object().value("entities");
        if (!entities.isArray()) return;
        const QSignalBlocker block(sceneList_);
        sceneList_->clear();
        new QListWidgetItem("Scene settings", sceneList_);
        for (const auto &id : entities.toArray()) {
            if (!id.isString()) continue;
            auto *item = new QListWidgetItem(id.toString(), sceneList_);
            item->setData(Qt::UserRole, id.toString());
        }
    }

    void focusDock(QDockWidget *dock) {
        dock->show();
        dock->raise();
        dock->window()->activateWindow();
        dock->widget()->setFocus();
    }

    void focusGuiNeighbor(QPoint direction) {
        constexpr auto center = [](QDockWidget *dock) { return dock->mapToGlobal(dock->rect().center()); };
        auto *focus = QApplication::focusWidget();
        const std::array docks{sceneDock_, documentDock_, viewportDock_};
        auto from = viewportDock_;
        for (auto *dock : docks)
            if (dock->isAncestorOf(focus)) from = dock;
        QDockWidget *next = nullptr;
        qint64 best = std::numeric_limits<qint64>::max();
        for (auto *dock : docks) {
            if (dock == from || !dock->isVisible()) continue;
            const auto delta = center(dock) - center(from);
            if (QPoint::dotProduct(delta, direction) <= 0) continue;
            const qint64 score = qint64(delta.x()) * delta.x() + qint64(delta.y()) * delta.y();
            if (score < best) { best = score; next = dock; }
        }
        if (next) focusDock(next);
    }

    void openGuiDocument(const QString &id) {
        if (!guiDocumentReady_ || !rpc_->is_ready()) return;
        documentDock_->show();
        documentDock_->raise();
        rpc_->notify("nvim_exec_autocmds", {"User", QVariantMap{{"pattern", "KalamGuiDocument"},
                                                               {"data", QVariantMap{{"id", id}}}}});
    }

    void runGuiCommand(QString command) {
        if (command.startsWith(':')) command.removeFirst();
        if (command == "viewport") { focusDock(viewportDock_); return; }
        if (!guiDocumentReady_ || !rpc_->is_ready()) {
            statusBar()->showMessage("Neovim is unavailable for document editing.", 3000);
            return;
        }
        if (command == "scene") {
            openGuiDocument({});
            focusDock(documentDock_);
        } else if (command == "form" || command == "text") {
            focusDock(documentDock_);
            rpc_->notify("nvim_command", {command == "form" ? "KalamMode gui" : "KalamMode text"});
        } else if (command == "entities") {
            rpc_->notify("nvim_command", {"KalamEntities"});
        } else if (command == "write") {
            auto *request = rpc_->call("nvim_command", {"wall"});
            connect(request, &Neovim_Rpc_Request::failed, this,
                    [this](quint32, const QVariant &, const QString &reason) { statusBar()->showMessage(reason, 6000); });
        } else if (!command.isEmpty()) {
            statusBar()->showMessage("Unknown command: " + command, 3000);
        }
    }

    void fail(const char *message) {
        std::fprintf(stderr, "%s\n", message);
        stop_ = true;
        result_ = 1;
        if (!running_)
            QApplication::exit(1);
    }

    void watchContent() {
        const auto files = watcher_.files();
        const auto directories = watcher_.directories();
        for (const auto &subdir : QStringList{"scenes", entityDirectory(), "materials", "meshes"}) {
            const QDir dir(root_ + "/" + subdir);
            if (dir.exists() && !directories.contains(dir.absolutePath()))
                watcher_.addPath(dir.absolutePath());
            for (const auto &file : dir.entryInfoList({"*.json", "*.glb"}, QDir::Files)) {
                if (!files.contains(file.absoluteFilePath()))
                    watcher_.addPath(file.absoluteFilePath());
            }
        }
    }

    void inspect(qint64 buffer, qint64 revision, const QByteArray &bytes, const QByteArray &entityRecords) {
        if (inspector_) {
            inspector_->raise();
            inspector_->activateWindow();
            return;
        }
        const auto manifest = QJsonDocument::fromJson(bytes).object();
        const auto records = QJsonDocument::fromJson(entityRecords).object();
        QJsonObject overrides{{scenePath(), QString::fromUtf8(bytes)}};
        for (auto it = records.begin(); it != records.end(); ++it)
            overrides.insert(entityPath(it.key()), it.value().toObject().value("text"));
        const auto error = validateSceneDocuments(scene_, overrides);
        if (!error.isEmpty()) {
            QMessageBox::warning(this, "Scene JSON", error);
            return;
        }
        auto document = manifest;
        QJsonArray entities;
        for (const auto &entry : manifest.value("entities").toArray()) {
            const auto text = records.value(entry.toString()).toObject().value("text").toString();
            entities.append(QJsonDocument::fromJson(text.toUtf8()).object());
        }
        document.insert("entities", entities);
        QFile file(":/kalam/entity-controls.json");
        if (!file.open(QIODevice::ReadOnly))
            return;
        const auto controls = QJsonDocument::fromJson(file.readAll()).array();
        QFile materials(root_ + "/materials/reference.json");
        if (!materials.open(QIODevice::ReadOnly))
            return;
        const auto reference = QJsonDocument::fromJson(materials.readAll()).object();
        inspector_ = new EntityInspector(document, controls, reference,
            [this, buffer, revision, manifest, records, overrides](const QByteArray &draft, EntityInspector *dialog) {
                auto candidate = overrides;
                auto nextManifest = manifest;
                QJsonArray ids;
                QVariantList edits;
                const auto document = QJsonDocument::fromJson(draft).object();
                for (const auto &entry : document.value("entities").toArray()) {
                    const auto entity = entry.toObject();
                    const auto id = entity.value("id").toString();
                    ids.append(id);
                    const auto old = records.value(id).toObject();
                    if (QJsonDocument::fromJson(old.value("text").toString().toUtf8()).object() == entity)
                        continue;
                    const auto relative = entityPath(id);
                    const auto text = QString::fromUtf8(QJsonDocument(entity).toJson());
                    candidate.insert(relative, text);
                    edits.append(QVariant(QVariantList{root_ + "/" + relative, old.value("buffer").toInteger(),
                                                       old.value("revision").toInteger(), text}));
                }
                if (ids != manifest.value("entities").toArray()) {
                    nextManifest.insert("entities", ids);
                    const auto text = QString::fromUtf8(QJsonDocument(nextManifest).toJson());
                    candidate.insert(scenePath(), text);
                    edits.append(QVariant(QVariantList{source_, buffer, revision, text}));
                }
                const auto error = validateSceneDocuments(scene_, candidate);
                if (!error.isEmpty()) {
                    dialog->showError(error);
                    return;
                }
                // Neovim owns the edit and the undo record. Reject concurrent changes.
                const auto lua = QStringLiteral(R"lua(
                    local source, revision, edits = ...
                    local function check(buf, tick)
                      if not vim.api.nvim_buf_is_valid(buf) or not vim.api.nvim_buf_is_loaded(buf)
                          or not vim.bo[buf].modifiable or vim.api.nvim_buf_get_changedtick(buf) ~= tick then
                        error("A buffer changed. Reopen the inspector.")
                      end
                    end
                    check(source, revision)
                    for _, edit in ipairs(edits) do
                      local path, buf, tick = unpack(edit)
                      if buf ~= 0 then
                        check(buf, tick)
                      elseif vim.uv.fs_stat(path) or vim.fn.bufnr(path) ~= -1 then
                        error("Entity file already exists: " .. path)
                      end
                    end
                    for _, edit in ipairs(edits) do
                      local path, buf = unpack(edit)
                      if buf == 0 then
                        vim.fn.mkdir(vim.fs.dirname(path), "p")
                        buf = vim.fn.bufadd(path)
                        vim.fn.bufload(buf)
                        vim.bo[buf].buflisted = true
                        edit[2] = buf
                      end
                    end
                    for _, edit in ipairs(edits) do
                      local _, buf, _, text = unpack(edit)
                      local lines = vim.split(text:gsub("\n$", ""), "\n", {plain = true})
                      vim.api.nvim_buf_set_lines(buf, 0, -1, false, lines)
                    end
                )lua");
                auto *request = rpc_->call("nvim_exec_lua",
                    {lua, QVariantList{buffer, revision, edits}});
                if (!request) {
                    dialog->showError("Neovim is disconnected.");
                    return;
                }
                connect(request, &Neovim_Rpc_Request::finished, dialog,
                        [dialog](quint32, const QVariant &) { dialog->accept(); });
                connect(request, &Neovim_Rpc_Request::failed, dialog,
                        [dialog](quint32, const QVariant &, const QString &reason) {
                            dialog->showError(reason);
                        });
            }, this);
        inspector_->move(mapToGlobal(QPoint(8, 32)));
        inspector_->open();
    }

    void initNeovim() {
        QFile lua(":/kalam/viewport.lua");
        if (!lua.open(QIODevice::ReadOnly)) {
            fail("Could not load viewport runtime");
            return;
        }
        auto *request = rpc_->call("nvim_exec_lua",
            {QString::fromUtf8(lua.readAll()), QVariantList{
             QVariant::fromValue(rpc_->channel_id()), source_, QVariantMap{
                 {"background", QApplication::palette().color(QPalette::Base).name()},
                 {"alternate", QApplication::palette().color(QPalette::AlternateBase).name()},
                 {"panel", QApplication::palette().color(QPalette::Window).name()},
                 {"text", QApplication::palette().color(QPalette::Text).name()},
                 {"muted", QApplication::palette().color(QPalette::PlaceholderText).name()},
                 {"accent", QApplication::palette().color(QPalette::Accent).name()},
                 {"selected", QApplication::palette().color(QPalette::Highlight).name()}
             }, gui_}});
        if (!request) {
            fail("Could not configure Neovim");
            return;
        }
        connect(request, &Neovim_Rpc_Request::failed, this,
            [this](quint32, const QVariant &, const QString &reason) {
                auto msg = reason.toUtf8();
                fail(msg.constData());
            });
        if (gui_)
            connect(request, &Neovim_Rpc_Request::finished, this, [this] { guiDocumentReady_ = true; });
    }

    void layoutViewport() {
        if (gui_) return;
        const QRect cells = grid_->model().window_screen_rect(viewportWindow_);
        if (!viewportWindow_ || !cells.isValid() || cells.width() < 8 || cells.height() < 3 ||
            grid_->model().window_is_floating(viewportWindow_)) {
            if (viewport_->hasFocus())
                grid_->setFocus();
            viewport_->hide();
            return;
        }
        const QSize cell = grid_->cell_size();
        viewport_->setGeometry(cells.x()*cell.width(), cells.y()*cell.height(),
                               cells.width()*cell.width(), cells.height()*cell.height());
        // The native HWND normally paints over its Qt parent. Cut out the
        // cells Neovim assigned to higher grids so its popups remain visible.
        QRegion visible(viewport_->rect());
        QSet<qint64> clipped;
        const auto &model = grid_->model();
        for (int row = cells.top(); row <= cells.bottom(); ++row)
            for (int column = cells.left(); column <= cells.right(); ++column) {
                const auto id = model.grid_at(row, column);
                const auto cover = model.grid_screen_rect(id);
                if (cover == cells || !cover.isValid() || clipped.contains(id)) continue;
                clipped.insert(id);
                const auto overlap = cover.intersected(cells).translated(-cells.topLeft());
                visible -= QRect(overlap.x()*cell.width(), overlap.y()*cell.height(),
                                 overlap.width()*cell.width(), overlap.height()*cell.height());
            }
        if (clipped.isEmpty()) {
            if (!viewport_->mask().isEmpty()) viewport_->clearMask();
        } else {
            // Qt interprets an empty mask as unmasked.
            if (visible.isEmpty()) visible = QRegion(-1, -1, 1, 1);
            if (viewport_->mask() != visible) viewport_->setMask(visible);
        }
        viewport_->show();
        viewport_->raise();
        if (viewportFocusPending_) {
            viewportFocusPending_ = false;
            viewport_->setFocus();
        }
        scheduleViewport();
    }

    void scheduleViewport() {
        if (!scheduled_ && !running_) {
            scheduled_ = true;
            QTimer::singleShot(0, this, [this] {
                scheduled_ = false;
                if (running_ || stop_ || !viewport_->isVisible())
                    return;
                running_ = true;
                const int status = kalam_embed(reinterpret_cast<void *>(viewport_->winId()),
                    [](void *context) { return static_cast<Editor *>(context)->pump(); },
                    this, scene_.constData());
                running_ = false;
                QApplication::exit(result_ ? result_ : status);
            });
        }
    }

    int pump() {
        QApplication::processEvents();
        if (stop_ || !isVisible())
            return -1;
        if (const auto *picked = kalam_embed_take_pick(); picked && *picked) {
            if (smoke_) smokePick_ = QString::fromUtf8(picked);
            if (gui_)
                openGuiDocument(QString::fromUtf8(picked));
            else
                rpc_->notify("nvim_exec_autocmds", {"User", QVariantMap{{"pattern", "KalamViewportPicked"},
                    {"data", QVariantMap{{"id", QString::fromUtf8(picked)}}}}});
        }
        const bool visible = viewport_->isVisible() && !viewport_->window()->isMinimized();
        if (gui_) {
            unsigned char selection[65];
            const auto count = kh_selected(selection, sizeof(selection));
            const auto *item = sceneList_->currentItem();
            if ((count || item) && (!item || item->data(Qt::UserRole).toString().toUtf8() !=
                   QByteArrayView(reinterpret_cast<const char *>(selection), count))) {
                const QSignalBlocker block(sceneList_);
                const auto matches = count ? sceneList_->findItems(
                    QString::fromUtf8(reinterpret_cast<const char *>(selection), count), Qt::MatchExactly)
                    : QList<QListWidgetItem *>{};
                sceneList_->setCurrentItem(matches.isEmpty() ? nullptr : matches.front());
            }
        }
        if (smoke_) {
            if (gui_) testGuiWorkspace(visible);
            else testLayout(visible);
        }
        if (stop_)
            return -1;
        if (!visible)
            return 0;
        const int reload = reload_ || liveReload_ ? 4 : 0;
        if (reload_) json_->resetPreview();
        if (reload) {
            auto documents = liveDocuments_;
            if (!draftPath_.isEmpty()) documents.insert(draftPath_, QString::fromUtf8(draftBytes_));
            const auto bytes = QJsonDocument(documents).toJson(QJsonDocument::Compact);
            kalam_preview_documents(bytes.constData(), size_t(bytes.size()));
        }
        reload_ = false;
        liveReload_ = false;
        return 1 | (viewport_->hasFocus() ? 2 : 0) | reload;
    }

    bool replaceSmokeEntity(const QByteArray &contents) {
        QSaveFile file(root_ + "/" + entityPath("centerpiece"));
        if (file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size() && file.commit())
            return true;
        fail("Could not write the smoke fixture");
        return false;
    }

    void testGuiWorkspace(bool visible) {
        const auto frames = kalam_embed_frames();
        if (stage_ == 0 && visible && frames >= 3 && guiDocumentReady_) {
            smokeGuiSurface_ = viewport_->winId();
            hiddenFrames_ = frames;
            stage_ = 1;
            viewportDock_->setFloating(true);
        } else if (stage_ == 1 && visible && frames >= hiddenFrames_ + 3) {
            if (!viewportDock_->isFloating() || viewport_->winId() != smokeGuiSurface_ ||
                !kalam_embed_draw_count()) {
                fail("Detached viewport lost its native render surface");
                return;
            }
            focusDock(viewportDock_);
            stage_ = 6;
        } else if (stage_ == 6 && viewport_->hasFocus()) {
            kalam_embed_drawn_view(smokeView_.data());
            QKeyEvent left(QEvent::KeyPress, Qt::Key_H, Qt::NoModifier, "h");
            QApplication::sendEvent(viewport_, &left);
            stage_ = 7;
        } else if (stage_ == 7) {
            std::array<float, 16> view{};
            kalam_embed_drawn_view(view.data());
            if (std::abs(view[12] - smokeView_[12]) < 0.001f) return;
            QKeyEvent left(QEvent::KeyRelease, Qt::Key_H, Qt::NoModifier, "h");
            QApplication::sendEvent(viewport_, &left);
            hiddenFrames_ = frames;
            stage_ = 2;
            viewportDock_->setFloating(false);
        } else if (stage_ == 2 && visible && frames >= hiddenFrames_ + 3) {
            if (viewportDock_->isFloating() || viewport_->winId() != smokeGuiSurface_ ||
                !kalam_embed_draw_count()) {
                fail("Re-docked viewport lost its native render surface");
                return;
            }
            std::puts("Qt floated the live bgfx viewport, accepted camera input, and re-docked it");
            focusDock(viewportDock_);
            stage_ = 8;
        } else if (stage_ == 8 && viewport_->hasFocus()) {
            QKeyEvent prefix(QEvent::KeyPress, Qt::Key_W, Qt::ControlModifier);
            QKeyEvent left(QEvent::KeyPress, Qt::Key_H, Qt::NoModifier, "h");
            QApplication::sendEvent(viewport_, &prefix);
            QApplication::sendEvent(viewport_, &left);
            if (viewport_->hasFocus()) {
                fail("Ctrl-W h did not move between Qt docks");
                return;
            }
            QKeyEvent colon(QEvent::KeyPress, Qt::Key_Colon, Qt::ShiftModifier, ":");
            QApplication::sendEvent(sceneList_, &colon);
            if (!command_->hasFocus()) {
                fail("The Scene pane did not open the Qt command line");
                return;
            }
            command_->setText("viewport");
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(command_, &enter);
            if (!viewport_->hasFocus()) {
                fail("The Qt command line did not focus the viewport");
                return;
            }
            std::puts("Ctrl-W navigation and the Qt command line moved focus between panes");
            if (scene_ != "entities") {
                stage_ = 5;
                smokeLua("vim.schedule(function() vim.cmd('qa!') end)");
                return;
            }
            stage_ = 9;
        } else if (stage_ == 9) {
            auto *form = findChild<QTreeWidget *>("kalam-json-form");
            if (!form) return;
            const auto rows = form->findItems("direction", Qt::MatchExactly | Qt::MatchRecursive);
            if (rows.isEmpty()) return;
            auto *box = form->itemWidget(rows.front(), 1);
            auto *line = box ? box->findChild<QLineEdit *>() : nullptr;
            if (!line) return;
            line->setText("4");
            QMetaObject::invokeMethod(line, "textEdited", Q_ARG(QString, line->text()));
            smokeLightInput_ = line;
            stage_ = 10;
        } else if (stage_ == 10 && kalam_embed_drawn_light_x() > 0.f) {
            if (!smokeLightInput_) { fail("Live preview replaced the active editor"); return; }
            smokeLightInput_->setFocus();
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(smokeLightInput_, &escape);
            stage_ = 11;
        } else if (stage_ == 11 && kalam_embed_drawn_light_x() < 0.f) {
            std::puts("Editing sun direction in the form updated the renderer before Enter or save; Escape restored it");
            const auto items = sceneList_->findItems("centerpiece", Qt::MatchExactly);
            if (items.isEmpty()) {
                fail("Scene dock did not list the centerpiece");
                return;
            }
            sceneList_->setCurrentItem(items.front());
            stage_ = 3;
        } else if (stage_ == 3) {
            auto *form = findChild<QTreeWidget *>("kalam-json-form");
            if (!form || form->headerItem()->text(0) != "centerpiece.json") return;
            unsigned char selection[65];
            const auto count = kh_selected(selection, sizeof(selection));
            if (QByteArrayView(reinterpret_cast<const char *>(selection), count) != "centerpiece") {
                fail("Scene dock did not select the corresponding live entity");
                return;
            }
            form->setFocus();
            QKeyEvent colon(QEvent::KeyPress, Qt::Key_Colon, Qt::ShiftModifier, ":");
            QApplication::sendEvent(form, &colon);
            stage_ = 12;
        } else if (stage_ == 12) {
            auto *form = findChild<QTreeWidget *>("kalam-json-form");
            if (!form || form->isVisible()) return;
            rpc_->notify("nvim_input", {"<Esc>"});
            stage_ = 13;
        } else if (stage_ == 13) {
            auto *form = findChild<QTreeWidget *>("kalam-json-form");
            if (!form || !form->isVisible()) return;
            std::puts("The form revealed Neovim's command line and returned on Escape");
            runGuiCommand("text");
            stage_ = 4;
        } else if (stage_ == 4 && !findChild<QTreeWidget *>("kalam-json-form")) {
            std::puts("Scene dock selection opened the matching document in Form and JSON views");
            initialDraws_ = kalam_embed_draw_count();
            stage_ = 14;
            smokeLua(R"lua(
                assert(vim.api.nvim_buf_get_name(0):match("centerpiece%.json$"))
                assert(vim.bo.syntax == "json")
                local text = table.concat(vim.api.nvim_buf_get_lines(0, 0, -1, false), "\n")
                local entity = vim.json.decode(text)
                entity.components.render = vim.NIL
                vim.api.nvim_buf_set_lines(0, 0, -1, false, {vim.json.encode(entity)})
                assert(vim.bo.modified)
            )lua");
        } else if (stage_ == 14 && kalam_embed_draw_count() + 2 == initialDraws_) {
            stage_ = 15;
            smokeLua("assert(vim.bo.modified); vim.cmd.undo()");
        } else if (stage_ == 15 && kalam_embed_draw_count() == initialDraws_) {
            std::puts("Unsaved entity JSON edits and Neovim undo both updated the live viewport");
            stage_ = 5;
            smokeLua("vim.schedule(function() vim.cmd('qa!') end)");
        }
    }

    void testLayout(bool visible) {
        const auto frames = kalam_embed_frames();
        if (stage_ == 0 && visible && frames >= 3) {
            smokeViewportWindow_ = viewportWindow_;
            smokeLua(R"lua(
                local win = vim.api.nvim_get_current_win()
                vim.g.kalam_smoke_text_win = win
                local prior = vim.api.nvim_win_get_buf(win)
                vim.g.kalam_smoke_text_buffer = prior
                assert(vim.bo.filetype == "json" and vim.bo.syntax == "json")
                assert(vim.fn.synIDattr(vim.fn.synID(2, 5, 1), "name") ~= "",
                    "The scene opens with JSON syntax highlighting")
                vim.api.nvim_feedkeys(vim.keycode(":viewport<CR>"), "mx", false)
                assert(vim.api.nvim_get_current_win() == win)
                assert(vim.bo.filetype == "kalam.viewport")
                assert(vim.api.nvim_win_get_buf(win) ~= prior)
            )lua");
            stage_ = 38;
        } else if (stage_ == 38 && visible && viewportWindow_ != smokeViewportWindow_ && viewport_->hasFocus()) {
            QKeyEvent colon(QEvent::KeyPress, Qt::Key_Colon, Qt::ShiftModifier, ":");
            QApplication::sendEvent(viewport_, &colon);
            rpc_->notify("nvim_input", {"viewport<CR>"});
            stage_ = 40;
        } else if (stage_ == 40 && !visible && viewportWindow_ == 0) {
            auto *request = rpc_->call("nvim_exec_lua", {R"lua(
                local old_viewport = ...
                local win = vim.g.kalam_smoke_text_win
                assert(vim.api.nvim_get_current_win() == win)
                assert(vim.api.nvim_win_get_buf(win) == vim.g.kalam_smoke_text_buffer,
                    "Toggling the viewport must restore the original buffer")
                vim.api.nvim_set_current_win(old_viewport)
                vim.cmd.Viewport()
                vim.api.nvim_set_current_win(win)
                vim.cmd("set guifont=Consolas:h16")
            )lua", QVariantList{smokeViewportWindow_}});
            connect(request, &Neovim_Rpc_Request::failed, this,
                    [this](quint32, const QVariant &, const QString &reason) {
                        fail(reason.toUtf8().constData());
                    });
            stage_ = 39;
        } else if (stage_ == 39 && visible && viewportWindow_ == smokeViewportWindow_) {
            std::puts(":viewport rendered in the current Neovim window and restored its JSON buffer");
            stage_ = -1;
        } else if (stage_ == -1 && grid_->grid_font().pointSize() == 16) {
            initial_ = viewport_->geometry();
            // The scratch split is on the right. Resize that window without
            // taking Neovim focus away from the source buffer.
            rpc_->call("nvim_exec_lua", {"local win = ...; vim.api.nvim_win_set_width(win, math.max(8, vim.api.nvim_win_get_width(win) - 7))",
                                        QVariantList{viewportWindow_}});
            stage_ = 1;
        } else if (stage_ == 1 && visible && viewport_->geometry() != initial_) {
            rpc_->call("nvim_command", {"tabnew"});
            stage_ = 2;
        } else if (stage_ == 2 && !visible) {
            hiddenFrames_ = frames;
            rpc_->call("nvim_command", {"tabprevious"});
            stage_ = 3;
        } else if (stage_ == 3 && visible && frames > hiddenFrames_) {
            rpc_->call("nvim_exec_lua", {"vim.api.nvim_win_close(..., false)",
                                        QVariantList{viewportWindow_}});
            stage_ = 4;
        } else if (stage_ == 4 && !visible) {
            hiddenFrames_ = frames;
            rpc_->call("nvim_command", {"KalamViewport"});
            stage_ = 5;
        } else if (stage_ == 5 && visible && frames > hiddenFrames_) {
            if (!viewport_->hasFocus()) return;
            QKeyEvent prefix(QEvent::KeyPress, Qt::Key_W, Qt::ControlModifier);
            QApplication::sendEvent(viewport_, &prefix);
            QKeyEvent left(QEvent::KeyPress, Qt::Key_H, Qt::NoModifier, "h");
            QApplication::sendEvent(viewport_, &left);
            stage_ = 30;
        } else if (stage_ == 30 && !viewportCurrent_) {
            rpc_->notify("nvim_input", {"<C-w>l"});
            stage_ = 6;
        } else if (stage_ == 6 && viewport_->hasFocus()) {
            auto *request = rpc_->call("nvim_exec_lua", {R"lua(
                local win = ...
                local buf = vim.api.nvim_create_buf(false, true)
                vim.api.nvim_buf_set_lines(buf, 0, -1, false, {"key hints"})
                vim.g.kalam_smoke_float = vim.api.nvim_open_win(buf, false, {
                  relative = "win", win = win, row = 1, col = 2, width = 18, height = 3,
                  focusable = false, style = "minimal", zindex = 50
                })
            )lua", QVariantList{viewportWindow_}});
            connect(request, &Neovim_Rpc_Request::failed, this, [this](quint32, const QVariant &, const QString &reason) {
                fail(reason.toUtf8().constData());
            });
            stage_ = 31;
        } else if (stage_ == 31 && !viewport_->mask().isEmpty()) {
            const auto cell = grid_->cell_size();
            if (viewport_->mask().contains(QPoint(cell.width() * 2, cell.height())) ||
                !viewport_->mask().contains(QPoint(0, 0))) {
                fail("Floating Neovim grid clipped the wrong viewport cells");
                return;
            }
            smokeLua("vim.api.nvim_win_close(vim.g.kalam_smoke_float, true)");
            stage_ = 32;
        } else if (stage_ == 32 && viewport_->mask().isEmpty() && viewport_->hasFocus()) {
            std::puts("Ctrl-W engaged the viewport and a floating Neovim grid clipped the native surface");
            if (scene_ == "entities") {
                QKeyEvent pick(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
                QApplication::sendEvent(viewport_, &pick);
                stage_ = 28;
                return;
            }
            stage_ = 28;
        } else if (stage_ == 28 && (scene_ != "entities" || !smokePick_.isEmpty())) {
            if (scene_ == "entities") {
                if (smokePick_ != "centerpiece") {
                    fail("Center ray did not select the centerpiece");
                    return;
                }
                smokeLua(R"lua(
                    local manifest = vim.fn.bufnr("entities.json")
                    local file = vim.fn.fnamemodify(vim.api.nvim_buf_get_name(manifest), ":r") .. "/centerpiece.json"
                    local win = vim.fn.bufwinid(file)
                    assert(win ~= -1, "Picking must open the entity's file")
                    vim.api.nvim_win_call(win, function()
                      assert(vim.bo.filetype == "json" and vim.bo.syntax == "json")
                      assert(vim.fn.synIDattr(vim.fn.synID(2, 5, 1), "name") ~= "",
                        "Picked entities open with JSON syntax highlighting")
                    end)
                )lua");
            }
            kalam_embed_drawn_view(smokeView_.data());
            QKeyEvent movement(QEvent::KeyPress, Qt::Key_L, Qt::NoModifier);
            QApplication::sendEvent(viewport_, &movement);
            stage_ = 26;
        } else if (stage_ == 26) {
            std::array<float, 16> view{};
            kalam_embed_drawn_view(view.data());
            if (std::abs(view[12] - smokeView_[12]) < 0.001f) return;
            QKeyEvent movement(QEvent::KeyRelease, Qt::Key_L, Qt::NoModifier);
            QApplication::sendEvent(viewport_, &movement);
            smokeView_ = view;
            QKeyEvent rotation(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
            QApplication::sendEvent(viewport_, &rotation);
            stage_ = 27;
        } else if (stage_ == 27) {
            std::array<float, 16> view{};
            kalam_embed_drawn_view(view.data());
            if (std::abs(view[2] - smokeView_[2]) < 0.001f) return;
            QKeyEvent rotation(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier);
            QApplication::sendEvent(viewport_, &rotation);
            QKeyEvent pick(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
            QApplication::sendEvent(viewport_, &pick);
            QKeyEvent prefix(QEvent::KeyPress, Qt::Key_W, Qt::ControlModifier);
            QApplication::sendEvent(viewport_, &prefix);
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(viewport_, &escape);
            QApplication::sendEvent(viewport_, &escape);
            unsigned char selection;
            if (!viewport_->hasFocus() || kh_selected(&selection, 1) != 0) {
                fail("Escape did not clear selection while keeping the viewport active");
                return;
            }
            hiddenFrames_ = frames;
            stage_ = 34;
        } else if (stage_ == 34 && frames > hiddenFrames_) {
            unsigned char selection;
            if (kh_selected(&selection, 1) != 0) {
                fail("A pick queued before Escape reselected an entity");
                return;
            }
            QKeyEvent prefix(QEvent::KeyPress, Qt::Key_W, Qt::ControlModifier);
            QApplication::sendEvent(viewport_, &prefix);
            QKeyEvent left(QEvent::KeyPress, Qt::Key_H, Qt::NoModifier, "h");
            QApplication::sendEvent(viewport_, &left);
            stage_ = 35;
        } else if (stage_ == 35 && !viewportCurrent_ && grid_->hasFocus()) {
            std::puts("Neovim font updated; viewport resized, moved, rotated and returned focus");
            if (scene_ != "entities") {
                rpc_->call("nvim_command", {"qa!"});
                stage_ = 12;
                return;
            }
            hiddenFrames_ = frames;
            stage_ = 29;
        } else if (stage_ == 29 && frames >= hiddenFrames_ + 2) {
            initialDraws_ = kalam_embed_draw_count();
            rpc_->call("nvim_command", {"KalamEntities"});
            stage_ = 7;
        } else if (stage_ == 7 && inspector_) {
            auto *entity = inspector_->findChild<QComboBox *>("entity-picker");
            auto *render = inspector_->findChild<QComboBox *>("component-render");
            auto *apply = inspector_->findChild<QPushButton *>("apply-entity-edit");
            if (!entity || !render || !apply || entity->findText("centerpiece") < 0) {
                fail("Could not find ECS inspector controls");
                return;
            }
            entity->setCurrentText("centerpiece");
            render->setCurrentText("Remove");
            apply->click();
            stage_ = 8;
        } else if (stage_ == 8 && !inspector_) {
            rpc_->call("nvim_exec_lua", {R"lua(
                local buf = vim.fn.bufnr(...)
                vim.api.nvim_buf_call(buf, function() vim.cmd.write() end)
            )lua", QVariantList{root_ + "/" + entityPath("centerpiece")}});
            stage_ = 9;
        } else if (stage_ == 9 && kalam_embed_draw_count() + 2 == initialDraws_) {
            rpc_->call("nvim_exec_lua", {R"lua(
                local buf = vim.fn.bufnr(...)
                vim.api.nvim_buf_call(buf, function() vim.cmd.undo(); vim.cmd.write() end)
            )lua", QVariantList{root_ + "/" + entityPath("centerpiece")}});
            stage_ = 10;
        } else if (stage_ == 10 && kalam_embed_draw_count() == initialDraws_) {
            std::puts("Qt component removal and Neovim undo both reloaded into the renderer");
            QFile file(root_ + "/" + entityPath("centerpiece"));
            if (!file.open(QIODevice::ReadOnly)) {
                fail("Could not read the ECS smoke fixture");
                return;
            }
            originalEntity_ = file.readAll();
            file.close();
            auto doc = QJsonDocument::fromJson(originalEntity_).object();
            auto components = doc.value("components").toObject();
            components.insert("render", QJsonValue::Null);
            doc.insert("components", components);
            if (replaceSmokeEntity(QJsonDocument(doc).toJson()))
                stage_ = 11;
        } else if (stage_ == 11 && kalam_embed_draw_count() + 2 == initialDraws_) {
            if (replaceSmokeEntity(originalEntity_))
                stage_ = 12;
        } else if (stage_ == 12 && scene_ == "entities" && kalam_embed_draw_count() == initialDraws_) {
            std::puts("External atomic edits to a per-entity JSON file reloaded twice");
            rpc_->call("nvim_exec_lua", {R"lua(
                local buf = vim.fn.bufnr("entities.json")
                local win = vim.fn.bufwinid(buf)
                if win == -1 then
                  vim.cmd("hide buffer " .. buf)
                  win = vim.api.nvim_get_current_win()
                else
                  vim.api.nvim_set_current_win(win)
                end
                local line = vim.fn.search('"exposure"', 'w')
                vim.api.nvim_win_set_cursor(win, {line, 3})
                vim.cmd.KalamField()
            )lua", QVariantList{}});
            stage_ = 13;
        } else if (stage_ == 13) {
            auto *field = findChild<QDialog *>("kalam-field-editor");
            if (!field || !field->isVisible()) return;
            auto *value = field->findChild<QLineEdit *>();
            auto *buttons = field->findChild<QDialogButtonBox *>();
            auto *slider = field->findChild<QSlider *>("kalam-exposure-slider");
            if (!value || !buttons || !slider) {
                fail("Expected a numeric exposure editor");
                return;
            }
            slider->setValue(150);
            stage_ = 18;
        } else if (stage_ == 18 && std::abs(kalam_embed_drawn_exposure() - 1.5f) < 0.001f) {
            auto *field = findChild<QDialog *>("kalam-field-editor");
            field->reject();
            stage_ = 19;
        } else if (stage_ == 19 && std::abs(kalam_embed_drawn_exposure() - 1.f) < 0.001f) {
            rpc_->call("nvim_command", {"KalamField"});
            stage_ = 20;
        } else if (stage_ == 20) {
            auto *field = findChild<QDialog *>("kalam-field-editor");
            if (!field || !field->isVisible()) return;
            auto *value = field->findChild<QLineEdit *>();
            auto *buttons = field->findChild<QDialogButtonBox *>();
            value->setText("1.5");
            buttons->button(QDialogButtonBox::Ok)->click();
            stage_ = 14;
        } else if (stage_ == 14 && !findChild<QDialog *>("kalam-field-editor")) {
            auto *request = rpc_->call("nvim_exec_lua", {R"lua(
                local buf = vim.fn.bufnr("entities.json")
                local function exposure()
                  return vim.json.decode(table.concat(vim.api.nvim_buf_get_lines(buf,0,-1,false),"\n")).exposure
                end
                assert(exposure() == 1.5)
                vim.api.nvim_buf_call(buf, function() vim.cmd.undo() end)
                assert(exposure() == 1)
                vim.cmd.split()
                local other = vim.api.nvim_create_buf(false, false)
                vim.bo[other].swapfile = false
                vim.api.nvim_buf_set_name(other, "other.json")
                vim.api.nvim_buf_set_lines(other,0,-1,false, {'{"matrix":[[1,0],[0,1]],"active":true}'})
                vim.api.nvim_win_set_buf(0, other)
                vim.bo[other].modified = false
                vim.cmd.KalamMode("gui")
            )lua", QVariantList{}});
            connect(request, &Neovim_Rpc_Request::failed, this,
                    [this](quint32, const QVariant &, const QString &message) {
                        fail(message.toUtf8().constData());
                    });
            stage_ = 15;
        } else if (stage_ == 15 && findChildren<QTreeWidget *>("kalam-json-form").size() == 2) {
            for (auto *tree : findChildren<QTreeWidget *>("kalam-json-form")) {
                for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                    auto *item = tree->topLevelItem(i);
                    if (item->childCount() && item->isExpanded()) {
                        item->setExpanded(false);
                        return;
                    }
                }
            }
            stage_ = 36;
        } else if (stage_ == 36 && findChildren<QTreeWidget *>("kalam-json-form").size() == 2) {
            const auto forms = findChildren<QTreeWidget *>("kalam-json-form");
            bool tested = false;
            for (auto *tree : forms) {
                const auto cameras = tree->findItems("camera", Qt::MatchExactly, 0);
                if (cameras.isEmpty()) continue;
                auto *camera = cameras.front();
                auto *target = camera->child(0);
                if (!target || target->text(0) != "target" || tree->itemWidget(target, 1)) {
                    fail("Collapsing a GUI section did not release its inline controls");
                    return;
                }
                camera->setExpanded(true);
                auto *box = tree->itemWidget(target, 1);
                if (!box || box->findChildren<QLineEdit *>().size() != 3) {
                    fail("Reopening a GUI section did not restore its vector editor");
                    return;
                }
                camera->setExpanded(false);
                tested = true;
            }
            if (!tested) {
                fail("Expected a camera section in the generated GUI");
                return;
            }
            std::puts("Collapsed all generated GUI sections and reopened an inline vector editor");
            for (auto *tree : forms) {
                const auto matrices = tree->findItems("matrix", Qt::MatchExactly, 0);
                if (matrices.isEmpty()) continue;
                auto *box = tree->itemWidget(matrices.front(), 1);
                const auto values = box ? box->findChildren<QLineEdit *>() : QList<QLineEdit *>{};
                if (values.size() != 4) {
                    fail("Expected four inline matrix value boxes");
                    return;
                }
                values.front()->setText("2");
                smokePendingInline_ = box;
                QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                QApplication::sendEvent(values.front(), &enter);
                stage_ = 33;
                return;
            }
            fail("Expected matrix in the generated form");
        } else if (stage_ == 33 && !smokePendingInline_ && findChildren<QTreeWidget *>("kalam-json-form").size() == 2) {
            for (auto *tree : findChildren<QTreeWidget *>("kalam-json-form")) {
                const auto toggles = tree->findItems("active", Qt::MatchExactly, 0);
                if (!toggles.isEmpty()) {
                    const auto matrices = tree->findItems("matrix", Qt::MatchExactly, 0);
                    auto *box = tree->itemWidget(matrices.front(), 1);
                    if (!box || !box->isEnabled() || box->findChild<QLineEdit *>()->text() != "2") return;
                    toggles.front()->setCheckState(1, Qt::Unchecked);
                }
            }
            auto *request = rpc_->call("nvim_exec_lua", {R"lua(
                local buf = vim.fn.bufnr("other.json")
                local current = vim.json.decode(table.concat(vim.api.nvim_buf_get_lines(buf,0,-1,false),"\n"))
                assert(current.active == false and current.matrix[1][1] == 2)
                vim.bo[buf].modified = false
                vim.cmd.KalamMode("text")
            )lua", QVariantList{}});
            connect(request, &Neovim_Rpc_Request::failed, this,
                    [this](quint32, const QVariant &, const QString &message) {
                        fail(message.toUtf8().constData());
                    });
            stage_ = 16;
        } else if (stage_ == 16 && findChildren<QTreeWidget *>("kalam-json-form").empty()) {
            std::puts("Context edit undid cleanly; two JSON panes switched to forms, edited and switched back");
            smokeLua(R"lua(
                local win = vim.fn.bufwinid("entities.json")
                vim.api.nvim_set_current_win(win)
                vim.cmd.KalamMode("tui")
                assert(vim.bo.filetype == "kalam.tui")
                local rows = table.concat(vim.api.nvim_buf_get_lines(0, 0, -1, false), "\n")
                assert(rows:find("Parameter", 1, true) and rows:find("Value", 1, true))
                assert(rows:find("target", 1, true) and rows:find("entities", 1, true))
                vim.cmd("normal l")
            )lua");
            stage_ = 21;
        } else if (stage_ == 21 && std::abs(kalam_embed_drawn_exposure() - 1.1f) < 0.001f) {
            smokeLua(R"lua(
                local buf = vim.fn.bufnr("entities.json")
                local text = table.concat(vim.api.nvim_buf_get_lines(buf, 0, -1, false), "\n")
                assert(vim.json.decode(text).exposure == 1)
                vim.api.nvim_feedkeys(string.char(27), "mx", false)
            )lua");
            stage_ = 22;
        } else if (stage_ == 22 && std::abs(kalam_embed_drawn_exposure() - 1.f) < 0.001f) {
            smokeLua(R"lua(
                vim.cmd("normal 2l")
                vim.api.nvim_feedkeys(string.char(13), "mx", false)
            )lua");
            stage_ = 23;
        } else if (stage_ == 23 && std::abs(kalam_embed_drawn_exposure() - 1.2f) < 0.001f) {
            smokeLua(R"lua(
                assert(vim.wait(1000, function()
                  vim.cmd.KalamMode("text")
                  return vim.bo.filetype ~= "kalam.tui"
                end))
                local function exposure()
                  return vim.json.decode(table.concat(vim.api.nvim_buf_get_lines(0, 0, -1, false), "\n")).exposure
                end
                assert(exposure() == 1.2)
                vim.cmd.undo()
                assert(exposure() == 1)
                vim.cmd.write()
            )lua");
            stage_ = 24;
        } else if (stage_ == 24 && std::abs(kalam_embed_drawn_exposure() - 1.f) < 0.001f) {
            std::puts("Qt and TUI exposure previews rendered, cancelled, applied, and undid");
            auto *request = smokeLua(R"lua(
                local scratch = vim.api.nvim_create_buf(false, true)
                vim.api.nvim_win_set_buf(0, scratch)
                vim.api.nvim_buf_set_lines(scratch, 0, -1, false, {
                  '{"version":1,"title":"Lighting","controls":[' ..
                  '{"label":"Exposure","bind":"/exposure","min":0.01,"max":32,"step":0.1},' ..
                  '{"label":"Sun red","bind":"/sun/radiance/0"},' ..
                  '{"label":"Sky","bind":"/unknown"}]}'
                })
                vim.cmd.KalamPanel("tui")
                local rows = vim.api.nvim_buf_get_lines(0, 0, -1, false)
                assert(rows[1] == "Lighting" and rows[4]:find("Sun red", 1, true))
                assert(rows[5]:find("Unresolved binding", 1, true))
                vim.cmd("normal j")
                assert(vim.api.nvim_win_get_cursor(0)[1] == 4)
                vim.cmd("normal l")
                vim.api.nvim_feedkeys(string.char(13), "mx", false)
                assert(vim.wait(1000, function()
                  vim.cmd.KalamMode("gui")
                  return vim.bo.filetype ~= "kalam.tui"
                end))
                local text = table.concat(vim.api.nvim_buf_get_lines(0, 0, -1, false), "\n")
                assert(vim.json.decode(text).sun.radiance[1] == 3.1)
                vim.cmd.undo()
                if vim.fn.executable("prettier") == 1 or vim.fn.executable("prettier.cmd") == 1 then
                  local buffer = vim.fn.bufnr("other.json")
                  vim.api.nvim_buf_call(buffer, function()
                    vim.api.nvim_buf_set_lines(0, 0, -1, false, {
                      '{"matrix":[[1,0],[0,1]],"active":false,"long":[' .. string.rep('12345,', 30) .. '0]}'
                    })
                    vim.cmd.KalamFormatJson()
                    assert(vim.api.nvim_buf_line_count(0) > 1)
                    vim.bo.modified = false
                  end)
                end
            )lua");
            stage_ = 37;
            connect(request, &Neovim_Rpc_Request::finished, this, [this] { stage_ = 25; });
        } else if (stage_ == 25) {
            const auto forms = findChildren<QTreeWidget *>("kalam-json-form");
            const auto lighting = std::find_if(forms.begin(), forms.end(), [](const QTreeWidget *tree) {
                return tree->headerItem()->text(0) == "Lighting";
            });
            if (lighting == forms.end()) return;
            if ((*lighting)->topLevelItemCount() != 3 || !(*lighting)->topLevelItem(2)->isDisabled()) {
                fail("Expected ordered panel fields and a disabled unresolved binding");
                return;
            }
            std::puts("Panel descriptor opened in TUI and Qt, edited a nested value, and undid");
            smokeLua("vim.schedule(function() vim.cmd('qa!') end)");
            stage_ = 17;
        }
    }

    Neovim_Rpc_Request *smokeLua(const QString &script) {
        auto *request = rpc_->call("nvim_exec_lua", {script, QVariantList{}});
        connect(request, &Neovim_Rpc_Request::failed, this,
                [this](quint32, const QVariant &, const QString &message) {
                    if (!stop_) fail(message.toUtf8().constData());
                });
        return request;
    }

    std::array<float, 16> smokeView_{};
    WId smokeGuiSurface_ = 0;
    qint64 smokeViewportWindow_ = 0;
    QString smokePick_;
    QPointer<QWidget> smokePendingInline_;
    QPointer<QLineEdit> smokeLightInput_;
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    installEditorTheme(app);
    const auto args = app.arguments();
    bool smoke = false, gui = false;
    if (args.size() < 3 || args.size() > 5) {
        std::fputs("usage: kalam-editor SCENE CONTENT_ROOT [--gui] [--smoke]\n", stderr);
        return 2;
    }
    for (const auto &option : args.sliced(3)) {
        if (option == "--smoke" && !smoke) smoke = true;
        else if (option == "--gui" && !gui) gui = true;
        else {
            std::fprintf(stderr, "Unknown or duplicate option: %s\n", qUtf8Printable(option));
            return 2;
        }
    }
    const QRegularExpression sceneName(QStringLiteral("^[a-zA-Z0-9_-]{1,64}$"));
    if (!sceneName.match(args[1]).hasMatch()) {
        std::fputs("Invalid scene name\n", stderr);
        return 2;
    }
    const QFileInfo content(args[2]);
    if (smoke && !content.absoluteDir().dirName().startsWith("editor-smoke-")) {
        std::fputs("Use cargo xtask editor --smoke to test against a temporary scene copy.\n", stderr);
        return 2;
    }
    const auto root = content.absoluteFilePath();
    qputenv("KALAM_CONTENT_ROOT", root.toUtf8());
    const auto error = validateSceneDocuments(args[1].toUtf8());
    if (!error.isEmpty()) {
        std::fprintf(stderr, "Kalam: %s\n", qUtf8Printable(error));
        return 1;
    }
    Editor editor(args[1], root, smoke, gui);
    editor.show();
    return app.exec();
}
