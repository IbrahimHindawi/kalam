#ifndef KALAM_EDITOR_QT_EDITOR_MAIN_WINDOW_H
#define KALAM_EDITOR_QT_EDITOR_MAIN_WINDOW_H

#include "editor_types.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>

namespace NeovimQt
{
class NeovimConnector;
}

namespace Kalam::Editor
{

class Author_Transaction_Controller;
class Build_Controller;
class Context_Popup;
class Editor_Neovim_Adapter;
class Editor_Neovim_Shell;
class Editor_Runtime_Host;
class Engine_Viewport_Widget;
class Generated_Form;

struct Editor_Configuration
{
    QString source_root;
    QString build_directory;
    QString canonical_module_dll;
    QString hot_copy_directory;
    QString neovim_runtime_directory;
    QString initial_file;
    QString ninja_program = QStringLiteral("ninja");
    bool force_warp = false;
    bool acceptance_smoke = false;
};

class Editor_Main_Window final : public QMainWindow
{
    Q_OBJECT

public:
    Editor_Main_Window(
        NeovimQt::NeovimConnector* connector,
        Editor_Neovim_Shell* shell,
        const Editor_Configuration& configuration,
        QWidget* parent = nullptr);
    ~Editor_Main_Window() override;

    void start_acceptance_smoke(
        const QString& cube_document,
        const QString& post_document,
        int timeout_milliseconds = 15000);
    void begin_acceptance_shutdown();

signals:
    void acceptanceSmokeFinished(bool succeeded, QString reason);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    Editor_Configuration configuration_;
    Editor_Neovim_Shell* shell_ = nullptr;
    Editor_Neovim_Adapter* neovim_ = nullptr;
    Editor_Runtime_Host* runtime_host_ = nullptr;
    Build_Controller* build_controller_ = nullptr;
    Author_Transaction_Controller* transactions_ = nullptr;
    Engine_Viewport_Widget* viewport_ = nullptr;
    QPointer<Context_Popup> context_popup_;
    QPointer<Generated_Form> generated_form_;
    Cursor_Geometry cursor_geometry_;
    quint64 selection_context_request_id_ = 0;
    Document_Handle selection_document_;
    QTimer smoke_timer_;
    QElapsedTimer smoke_clock_;
    QString smoke_cube_document_;
    QString smoke_post_document_;
    int smoke_timeout_milliseconds_ = 0;
    bool smoke_documents_applied_ = false;
    quint64 smoke_frame_floor_ = 0;
    bool close_authorized_ = false;
    bool shutting_down_ = false;

    void wire_editor();
    void open_context_popup(
        const QVector<Field_Descriptor>& fields,
        const QVector<Author_Value_Owned>& values,
        const Source_Context& context);
    void open_generated_form(const Buffer_Snapshot& snapshot);
    void refresh_generated_form();
    QVector<Author_Value_Owned> read_form_values(
        const Document_Handle& document,
        const Schema_Descriptor& schema,
        QString* error) const;
    void handle_viewport_interaction(
        const Viewport_Interaction_Result& result);
    void handle_selection_context(
        quint64 request_id,
        bool succeeded,
        const Source_Context& context,
        const QString& error);
    void run_smoke_step();
    bool apply_smoke_document(
        const QString& path,
        qint64 synthetic_buffer,
        QString* error);
    void finish_smoke(bool succeeded, const QString& reason);
    void shutdown_editor(bool force_neovim = false);
};

} // namespace Kalam::Editor

#endif
