#include "editor_main_window.h"

#include "author_transaction_controller.h"
#include "build_controller.h"
#include "context_popup.h"
#include "editor_neovim_adapter.h"
#include "editor_neovim_shell.h"
#include "editor_runtime_host.h"
#include "engine_viewport_widget.h"
#include "generated_form.h"

#include <QCloseEvent>
#include <QFile>
#include <QFileInfo>
#include <QSplitter>
#include <QtMath>

#include "neovimconnector.h"

namespace Kalam::Editor
{

Editor_Main_Window::Editor_Main_Window(
    NeovimQt::NeovimConnector* connector,
    Editor_Neovim_Shell* shell,
    const Editor_Configuration& configuration,
    QWidget* parent)
    : QMainWindow(parent), configuration_(configuration), shell_(shell)
{
    Q_ASSERT(connector != nullptr);
    Q_ASSERT(shell != nullptr);
    setObjectName(QStringLiteral("editorMainWindow"));
    setWindowTitle(QStringLiteral("Kalam"));
    resize(1440, 900);

    runtime_host_ = new Editor_Runtime_Host(this);
    runtime_host_->set_project_root(configuration_.source_root);
    runtime_host_->set_force_warp(configuration_.force_warp);
    runtime_host_->set_acceptance_smoke_mode(configuration_.acceptance_smoke);
    neovim_ = new Editor_Neovim_Adapter(connector, shell, this);
    build_controller_ = new Build_Controller(this);
    build_controller_->configure(
        configuration_.build_directory,
        configuration_.canonical_module_dll,
        configuration_.ninja_program);
    transactions_ = new Author_Transaction_Controller(
        runtime_host_,
        neovim_,
        this);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("editorSplitter"));
    shell_->setObjectName(QStringLiteral("neovimEditorRegion"));
    viewport_ = new Engine_Viewport_Widget(runtime_host_, splitter);
    viewport_->setObjectName(QStringLiteral("engineViewport"));
    splitter->addWidget(shell_);
    splitter->addWidget(viewport_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes(QList<int>{850, 590});
    setCentralWidget(splitter);

    wire_editor();
    neovim_->initialize_runtime(configuration_.neovim_runtime_directory);
    if (!configuration_.initial_file.isEmpty())
    {
        neovim_->open_file(configuration_.initial_file);
    }

    if (QFileInfo::exists(configuration_.canonical_module_dll))
    {
        QString error;
        if (!runtime_host_->queue_module_reload(
                1,
                configuration_.canonical_module_dll,
                configuration_.hot_copy_directory,
                &error))
        {
            neovim_->notify_user(error, true);
        }
        build_controller_->set_generation_floor(1);
    }

    smoke_timer_.setInterval(25);
    connect(
        &smoke_timer_,
        &QTimer::timeout,
        this,
        &Editor_Main_Window::run_smoke_step);
}

Editor_Main_Window::~Editor_Main_Window()
{
    shutdown_editor();
}

void Editor_Main_Window::wire_editor()
{
    connect(
        shell_,
        &Editor_Neovim_Shell::neovimGuiCloseRequest,
        this,
        [this](int)
        {
            close_authorized_ = true;
            close();
        });
    connect(
        shell_,
        &Editor_Neovim_Shell::cursorGeometryChanged,
        this,
        [this](Cursor_Geometry geometry)
        {
            cursor_geometry_ = geometry;
            if (context_popup_ != nullptr)
            {
                context_popup_->anchor_to(shell_, geometry.cursor_rect);
            }
        });
    connect(
        neovim_,
        &Editor_Neovim_Adapter::contextEditRequested,
        this,
        [this](const Source_Context& context)
        {
            QString error;
            if (!transactions_->begin_context_edit(context, &error))
            {
                neovim_->notify_user(error, true);
            }
        });
    connect(
        neovim_,
        &Editor_Neovim_Adapter::formEditRequested,
        this,
        &Editor_Main_Window::open_generated_form);
    connect(
        neovim_,
        &Editor_Neovim_Adapter::buildReloadRequested,
        build_controller_,
        &Build_Controller::request_build);
    connect(
        transactions_,
        &Author_Transaction_Controller::contextEditReady,
        this,
        &Editor_Main_Window::open_context_popup);
    connect(
        transactions_,
        &Author_Transaction_Controller::transactionError,
        this,
        [this](const QString& error)
        {
            neovim_->notify_user(error, true);
        });
    connect(
        transactions_,
        &Author_Transaction_Controller::transactionClosed,
        this,
        [this](bool)
        {
            if (context_popup_ != nullptr)
            {
                context_popup_->close();
            }
            neovim_->focus_editor();
        });

    connect(
        neovim_,
        &Editor_Neovim_Adapter::validDocumentReceived,
        this,
        [this](const Buffer_Snapshot& snapshot)
        {
            QString error;
            if (runtime_host_->accept_valid_document(snapshot, &error))
            {
                transactions_->document_changed(snapshot);
                refresh_generated_form();
            }
            else
            {
                neovim_->notify_user(error, true);
            }
        });
    connect(
        neovim_,
        &Editor_Neovim_Adapter::invalidDocumentReceived,
        this,
        [this](const Buffer_Snapshot& snapshot, const QString& message)
        {
            transactions_->document_changed(snapshot);
            Diagnostic diagnostic;
            diagnostic.file = snapshot.path;
            diagnostic.line = 1;
            diagnostic.column = 1;
            diagnostic.type = QStringLiteral("error");
            diagnostic.message = message;
            neovim_->set_quickfix(
                QVector<Diagnostic>{diagnostic},
                QStringLiteral("Authored JSON"),
                false);
        });
    connect(
        build_controller_,
        &Build_Controller::quickfixReady,
        neovim_,
        &Editor_Neovim_Adapter::set_quickfix);
    connect(
        build_controller_,
        &Build_Controller::moduleBuilt,
        this,
        [this](quint64 generation, const QString& module_path)
        {
            QString error;
            if (!runtime_host_->queue_module_reload(
                    generation,
                    module_path,
                    configuration_.hot_copy_directory,
                    &error))
            {
                neovim_->notify_user(error, true);
            }
        });
    connect(
        build_controller_,
        &Build_Controller::buildFailed,
        this,
        [this](quint64, const QString& summary)
        {
            neovim_->notify_user(summary, true);
        });
    connect(
        runtime_host_,
        &Editor_Runtime_Host::aboutToCommitModuleReload,
        viewport_,
        &Engine_Viewport_Widget::cancel_active_gizmo);
    connect(
        runtime_host_,
        &Editor_Runtime_Host::moduleReloadFailed,
        this,
        [this](quint64, const QString& error)
        {
            if (smoke_timer_.isActive())
            {
                finish_smoke(false, error);
            }
            Diagnostic diagnostic;
            diagnostic.type = QStringLiteral("error");
            diagnostic.message = error;
            neovim_->set_quickfix(
                QVector<Diagnostic>{diagnostic},
                QStringLiteral("Kalam module reload"),
                false);
            neovim_->notify_user(error, true);
        });
    connect(
        runtime_host_,
        &Editor_Runtime_Host::moduleReloaded,
        this,
        [this](quint64 generation)
        {
            neovim_->notify_user(
                QStringLiteral("game module generation %1 loaded")
                    .arg(generation),
                false);
        });
    connect(
        runtime_host_,
        &Editor_Runtime_Host::schemasChanged,
        this,
        [this]()
        {
            if (generated_form_ != nullptr)
            {
                generated_form_->close();
            }
        });
    connect(
        runtime_host_,
        &Editor_Runtime_Host::frameError,
        this,
        [this](const QString& error)
        {
            if (smoke_timer_.isActive())
            {
                finish_smoke(false, error);
            }
            neovim_->notify_user(error, true);
        });
    connect(
        runtime_host_,
        &Editor_Runtime_Host::viewportDeviceLost,
        this,
        [this](const QString& error)
        {
            if (smoke_timer_.isActive())
            {
                finish_smoke(false, error);
            }
            neovim_->notify_user(error, true);
        });
    connect(
        viewport_,
        &Engine_Viewport_Widget::viewportError,
        this,
        [this](const QString& error)
        {
            neovim_->notify_user(error, true);
        });
    connect(
        viewport_,
        &Engine_Viewport_Widget::viewportInteraction,
        this,
        &Editor_Main_Window::handle_viewport_interaction);
    connect(
        neovim_,
        &Editor_Neovim_Adapter::contextForPathFinished,
        this,
        &Editor_Main_Window::handle_selection_context);
    connect(
        neovim_,
        &Editor_Neovim_Adapter::disconnected,
        this,
        [this](const QString& reason)
        {
            if (smoke_timer_.isActive())
            {
                finish_smoke(false, reason);
            }
        });
}

void Editor_Main_Window::open_context_popup(
    const QVector<Field_Descriptor>& fields,
    const QVector<Author_Value_Owned>& values,
    const Source_Context& context)
{
    Q_UNUSED(context);
    if (context_popup_ != nullptr)
    {
        context_popup_->close();
    }
    context_popup_ = new Context_Popup(fields, values, this);
    connect(
        context_popup_,
        &Context_Popup::fieldPreviewRequested,
        this,
        [this](Author_Field_Id field_id, const Author_Value_Owned& value)
        {
            QString error;
            if (!transactions_->preview_field(field_id, value, &error))
            {
                neovim_->notify_user(error, true);
            }
        });
    connect(
        context_popup_,
        &Context_Popup::acceptRequested,
        transactions_,
        &Author_Transaction_Controller::accept);
    connect(
        context_popup_,
        &Context_Popup::cancelRequested,
        transactions_,
        &Author_Transaction_Controller::cancel);
    context_popup_->anchor_to(shell_, cursor_geometry_.cursor_rect);
    context_popup_->show();
    context_popup_->raise();
    context_popup_->activateWindow();
}

void Editor_Main_Window::open_generated_form(
    const Buffer_Snapshot& snapshot)
{
    QString error;
    if (!runtime_host_->accept_valid_document(snapshot, &error))
    {
        neovim_->notify_user(error, true);
        return;
    }
    const Document_Handle document =
        runtime_host_->document_for_buffer(snapshot.buffer);
    Schema_Descriptor schema;
    if (!runtime_host_->schema_for_document(document.document_id, &schema) ||
        (schema.flags & Author_Schema_Flag_Full_Form_Supported) == 0)
    {
        neovim_->notify_user(
            QStringLiteral("the active schema does not support a full form"),
            true);
        return;
    }
    const QVector<Author_Value_Owned> values =
        read_form_values(document, schema, &error);
    if (values.size() != schema.fields.size())
    {
        neovim_->notify_user(error, true);
        return;
    }

    if (generated_form_ != nullptr)
    {
        generated_form_->close();
    }
    generated_form_ = new Generated_Form(document, schema, values, this);
    connect(
        generated_form_,
        &Generated_Form::fieldPreviewRequested,
        this,
        [this](
            const Document_Handle& document,
            const Field_Descriptor& field,
            const Author_Value_Owned& original,
            const Author_Value_Owned& value)
        {
            QString error;
            if (!transactions_->begin_form_field_edit(
                    document, field, original, &error) ||
                !transactions_->preview_field(field.id, value, &error))
            {
                neovim_->notify_user(error, true);
            }
        });
    connect(
        generated_form_,
        &Generated_Form::fieldCommitRequested,
        this,
        [this](
            const Document_Handle& document,
            const Field_Descriptor& field,
            const Author_Value_Owned& original,
            const Author_Value_Owned& value)
        {
            QString error;
            if (!transactions_->begin_form_field_edit(
                    document, field, original, &error) ||
                !transactions_->preview_field(field.id, value, &error))
            {
                neovim_->notify_user(error, true);
                return;
            }
            transactions_->accept();
        });
    connect(
        generated_form_,
        &Generated_Form::cancelAllRequested,
        transactions_,
        &Author_Transaction_Controller::cancel);
    generated_form_->show();
    generated_form_->raise();
    generated_form_->activateWindow();
}

void Editor_Main_Window::refresh_generated_form()
{
    if (generated_form_ == nullptr)
    {
        return;
    }
    const Document_Handle old_document = generated_form_->document();
    const Document_Handle document =
        runtime_host_->document_for_buffer(old_document.buffer);
    if (!document.is_valid() ||
        document.document_id != old_document.document_id)
    {
        generated_form_->close();
        return;
    }

    Schema_Descriptor schema;
    QString error;
    if (!runtime_host_->schema_for_document(document.document_id, &schema))
    {
        generated_form_->close();
        return;
    }
    const QVector<Author_Value_Owned> values =
        read_form_values(document, schema, &error);
    if (values.size() != schema.fields.size())
    {
        generated_form_->close();
        neovim_->notify_user(error, true);
        return;
    }
    generated_form_->refresh(document, values);
}

QVector<Author_Value_Owned> Editor_Main_Window::read_form_values(
    const Document_Handle& document,
    const Schema_Descriptor& schema,
    QString* error) const
{
    QVector<Author_Value_Owned> values;
    values.reserve(schema.fields.size());
    for (const Field_Descriptor& field : schema.fields)
    {
        Author_Value_Owned value;
        if (!runtime_host_->read_author_field(
                document.document_id,
                document.root_object_id,
                field.id,
                &value,
                error))
        {
            return {};
        }
        values.push_back(value);
    }
    return values;
}

void Editor_Main_Window::handle_viewport_interaction(
    const Viewport_Interaction_Result& result)
{
    if ((result.flags & Game_Viewport_Result_Selection_Changed) != 0 &&
        (result.flags & Game_Viewport_Result_Has_Selection) != 0)
    {
        selection_document_ = runtime_host_->document_by_id(result.document_id);
        const QString source_path = runtime_host_->source_path_for_object(
            result.document_id,
            result.object_id);
        if (selection_document_.is_valid() && !source_path.isEmpty())
        {
            selection_context_request_id_ =
                neovim_->request_context_for_path(
                    selection_document_.buffer,
                    source_path);
        }
    }

    if ((result.flags & Game_Viewport_Result_Gizmo_Active) != 0 &&
        (result.flags & Game_Viewport_Result_Value_Changed) == 0 &&
        !transactions_->is_active())
    {
        QString error;
        if (!transactions_->begin_gizmo_edit(result, &error))
        {
            neovim_->notify_user(error, true);
        }
    }
    if ((result.flags & Game_Viewport_Result_Value_Changed) != 0)
    {
        transactions_->note_gizmo_value(result);
    }
    if ((result.flags & Game_Viewport_Result_Commit_Requested) != 0)
    {
        transactions_->accept_gizmo(result);
    }
    if ((result.flags & Game_Viewport_Result_Cancelled) != 0)
    {
        transactions_->note_gizmo_cancelled();
    }
}

void Editor_Main_Window::handle_selection_context(
    quint64 request_id,
    bool succeeded,
    const Source_Context& context,
    const QString& error)
{
    if (request_id != selection_context_request_id_)
    {
        return;
    }
    selection_context_request_id_ = 0;
    if (!succeeded)
    {
        neovim_->notify_user(error, true);
        return;
    }
    neovim_->open_file_location(
        selection_document_.path,
        context.range.start_row,
        context.range.start_column);
    neovim_->focus_editor();
}

void Editor_Main_Window::start_acceptance_smoke(
    const QString& cube_document,
    const QString& post_document,
    int timeout_milliseconds)
{
    if (smoke_timer_.isActive())
    {
        finish_smoke(false, QStringLiteral("acceptance smoke already active"));
        return;
    }
    smoke_cube_document_ = QFileInfo(cube_document).absoluteFilePath();
    smoke_post_document_ = QFileInfo(post_document).absoluteFilePath();
    smoke_timeout_milliseconds_ = qMax(1000, timeout_milliseconds);
    smoke_documents_applied_ = false;
    smoke_frame_floor_ = 0;
    smoke_clock_.start();
    smoke_timer_.start();
    QTimer::singleShot(
        0,
        this,
        &Editor_Main_Window::run_smoke_step);
}

void Editor_Main_Window::run_smoke_step()
{
    if (!smoke_timer_.isActive())
    {
        return;
    }
    if (smoke_clock_.elapsed() >= smoke_timeout_milliseconds_)
    {
        finish_smoke(
            false,
            QStringLiteral("timed out waiting for the integrated editor"));
        return;
    }

    QSplitter* splitter = qobject_cast<QSplitter*>(centralWidget());
    if (splitter == nullptr || splitter->count() != 2 ||
        splitter->widget(0) != shell_ ||
        splitter->widget(1) != viewport_)
    {
        finish_smoke(
            false,
            QStringLiteral("editor layout is not the required two-region splitter"));
        return;
    }

    if (!neovim_->is_ready() ||
        !shell_->isNeovimAttached() ||
        !runtime_host_->viewport_initialized() ||
        !runtime_host_->has_callable_module() ||
        runtime_host_->active_module_generation() == 0 ||
        runtime_host_->schemas().size() != 2 ||
        runtime_host_->rendered_frame_count() < 2 ||
        viewport_->physical_size().isEmpty())
    {
        return;
    }

    if (!smoke_documents_applied_)
    {
        QString error;
        if (!apply_smoke_document(smoke_cube_document_, 1001, &error))
        {
            finish_smoke(false, error);
            return;
        }
        const Document_Handle first_scene =
            runtime_host_->document_for_buffer(1001);
        if (!apply_smoke_document(smoke_cube_document_, 1001, &error))
        {
            finish_smoke(false, error);
            return;
        }
        const Document_Handle scene = runtime_host_->document_for_buffer(1001);
        if (!first_scene.is_valid() || !scene.is_valid() ||
            scene.document_id != first_scene.document_id ||
            scene.root_object_id != first_scene.root_object_id ||
            scene.revision <= first_scene.revision)
        {
            finish_smoke(
                false,
                QStringLiteral("root authored identity was not stable across revisions"));
            return;
        }
        if (!apply_smoke_document(smoke_post_document_, 1002, &error))
        {
            finish_smoke(false, error);
            return;
        }

        Schema_Descriptor scene_schema;
        if (!runtime_host_->schema_for_document(
                scene.document_id,
                &scene_schema))
        {
            finish_smoke(false, QStringLiteral("scene author schema unavailable"));
            return;
        }
        Field_Descriptor transform_field;
        bool found_transform = false;
        for (const Field_Descriptor& field : scene_schema.fields)
        {
            if (field.json_path == QByteArrayLiteral("$.cube.transform"))
            {
                transform_field = field;
                found_transform = true;
                break;
            }
        }
        if (!found_transform)
        {
            finish_smoke(false, QStringLiteral("scene transform field unavailable"));
            return;
        }

        Author_Value_Owned original;
        if (!runtime_host_->read_author_field(
                scene.document_id,
                scene.root_object_id,
                transform_field.id,
                &original,
                &error))
        {
            finish_smoke(false, error);
            return;
        }
        Author_Value_Owned preview = original;
        preview.transform.translation[0] += 0.75f;
        if (!runtime_host_->preview_author_field(
                scene.document_id,
                scene.root_object_id,
                transform_field.id,
                preview,
                &error))
        {
            finish_smoke(false, error);
            return;
        }
        Author_Value_Owned preview_readback;
        if (!runtime_host_->read_author_field(
                scene.document_id,
                scene.root_object_id,
                transform_field.id,
                &preview_readback,
                &error) ||
            qAbs(preview_readback.transform.translation[0] -
                 preview.transform.translation[0]) > 0.0001f)
        {
            finish_smoke(
                false,
                error.isEmpty()
                    ? QStringLiteral("author preview did not reach the module")
                    : error);
            return;
        }
        if (!runtime_host_->rehydrate_document(scene.document_id, &error))
        {
            finish_smoke(false, error);
            return;
        }
        Author_Value_Owned restored;
        if (!runtime_host_->read_author_field(
                scene.document_id,
                scene.root_object_id,
                transform_field.id,
                &restored,
                &error) ||
            qAbs(restored.transform.translation[0] -
                 original.transform.translation[0]) > 0.0001f)
        {
            finish_smoke(
                false,
                error.isEmpty()
                    ? QStringLiteral("fresh-revision rehydrate did not restore JSON")
                    : error);
            return;
        }

        const QSize viewport_size = viewport_->physical_size();
        Viewport_Interaction_Result selection;
        if (!runtime_host_->viewport_interaction(
                Game_Viewport_Interaction_Select,
                QPoint(viewport_size.width() / 2, viewport_size.height() / 2),
                {},
                viewport_size,
                &selection,
                &error) ||
            (selection.flags & Game_Viewport_Result_Has_Selection) == 0 ||
            selection.document_id != scene.document_id ||
            selection.object_id != scene.root_object_id ||
            runtime_host_->source_path_for_object(
                selection.document_id,
                selection.object_id) != QStringLiteral("$.cube"))
        {
            finish_smoke(
                false,
                error.isEmpty()
                    ? QStringLiteral("viewport selection did not map to the root cube")
                    : error);
            return;
        }

        smoke_documents_applied_ = true;
        smoke_frame_floor_ = runtime_host_->rendered_frame_count();
        return;
    }

    if (runtime_host_->rendered_frame_count() < smoke_frame_floor_ + 3)
    {
        return;
    }
    finish_smoke(
        true,
        QStringLiteral("integrated Neovim, WARP viewport, documents, selection, "
                       "preview rollback, and rendering passed"));
}

bool Editor_Main_Window::apply_smoke_document(
    const QString& path,
    qint64 synthetic_buffer,
    QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("could not read smoke document %1: %2")
                .arg(path, file.errorString());
        }
        return false;
    }
    const Document_Handle current =
        runtime_host_->document_for_buffer(synthetic_buffer);
    Buffer_Snapshot snapshot;
    snapshot.buffer = synthetic_buffer;
    snapshot.changedtick = current.is_valid()
        ? current.changedtick + 1
        : 1;
    snapshot.path = QFileInfo(path).absoluteFilePath();
    snapshot.text_utf8 = file.readAll();
    return runtime_host_->accept_valid_document(snapshot, error);
}

void Editor_Main_Window::finish_smoke(
    bool succeeded,
    const QString& reason)
{
    if (!smoke_timer_.isActive())
    {
        return;
    }
    smoke_timer_.stop();
    smoke_clock_.invalidate();
    emit acceptanceSmokeFinished(succeeded, reason);
}

void Editor_Main_Window::begin_acceptance_shutdown()
{
    shutdown_editor(true);
}

void Editor_Main_Window::closeEvent(QCloseEvent* event)
{
    if (shutting_down_)
    {
        event->accept();
        return;
    }
    if (close_authorized_ || !neovim_->is_ready())
    {
        shutdown_editor();
        event->accept();
        return;
    }
    if (shell_ != nullptr && shell_->close())
    {
        close_authorized_ = true;
        shutdown_editor();
        event->accept();
        return;
    }
    event->ignore();
}

void Editor_Main_Window::shutdown_editor(bool force_neovim)
{
    if (shutting_down_)
    {
        return;
    }
    shutting_down_ = true;
    smoke_timer_.stop();
    viewport_->cancel_active_gizmo();
    transactions_->cancel();
    build_controller_->shutdown();
    runtime_host_->shutdown();
    if (!close_authorized_ || force_neovim)
    {
        neovim_->request_neovim_close(force_neovim);
    }
}

} // namespace Kalam::Editor
