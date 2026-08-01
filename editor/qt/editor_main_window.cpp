#include "editor_main_window.h"

#include "author_transaction_controller.h"
#include "build_controller.h"
#include "context_popup.h"
#include "editor_neovim_adapter.h"
#include "editor_neovim_shell.h"
#include "editor_runtime_host.h"
#include "engine_viewport_widget.h"
#include "generated_form.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QtMath>

#include "neovim_rpc_client.h"

namespace Kalam::Editor
{

Editor_Main_Window::Editor_Main_Window(
    Neovim_Rpc_Client* client,
    Editor_Neovim_Shell* shell,
    const Editor_Configuration& configuration,
    QWidget* parent)
    : QMainWindow(parent), configuration_(configuration), shell_(shell)
{
    Q_ASSERT(client != nullptr);
    Q_ASSERT(shell != nullptr);
    setObjectName(QStringLiteral("editorMainWindow"));
    setWindowTitle(QStringLiteral("Kalam"));
    resize(1440, 900);

    runtime_host_ = new Editor_Runtime_Host(this);
    runtime_host_->set_project_root(configuration_.source_root);
    runtime_host_->set_force_warp(configuration_.force_warp);
    runtime_host_->set_acceptance_smoke_mode(configuration_.acceptance_smoke);
    neovim_ = new Editor_Neovim_Adapter(client, shell, this);
    build_controller_ = new Build_Controller(this);
    build_controller_->configure(
        configuration_.build_directory,
        configuration_.canonical_module_dll,
        configuration_.ninja_program);
    transactions_ = new Author_Transaction_Controller(
        runtime_host_,
        neovim_,
        this);

    /*
     * The buffer-hosted viewport layout (docs/buffer_hosted_viewport.md
     * section 4): the Neovim grid surface fills the central widget, and
     * the engine viewport is a native-window child overlaid on whichever
     * Neovim window shows kalam://viewport. The viewport widget is never
     * reparented; its HWND feeds the D3D11 swap chain.
     */
    shell_->setObjectName(QStringLiteral("neovimEditorRegion"));
    setCentralWidget(shell_);
    viewport_ = new Engine_Viewport_Widget(runtime_host_, shell_);
    viewport_->setObjectName(QStringLiteral("engineViewport"));
    viewport_->hide();

    wire_editor();
    QTimer::singleShot(
        0,
        neovim_,
        &Editor_Neovim_Adapter::focus_editor);

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
            project_context_popup();
        });
    connect(
        shell_,
        &Neovim_Grid_Widget::gridLayoutChanged,
        this,
        &Editor_Main_Window::update_viewport_overlay);
    connect(
        neovim_,
        &Editor_Neovim_Adapter::viewportWindowsChanged,
        this,
        [this](const QVector<qlonglong>& windows, qlonglong active_window)
        {
            viewport_windows_ = windows;
            active_viewport_window_ = active_window;
            update_viewport_overlay();
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
        neovim_,
        &Editor_Neovim_Adapter::focusEditorRequested,
        neovim_,
        &Editor_Neovim_Adapter::focus_editor);
    connect(
        neovim_,
        &Editor_Neovim_Adapter::focusViewportRequested,
        this,
        &Editor_Main_Window::focus_viewport);
    connect(
        neovim_,
        &Editor_Neovim_Adapter::toggleViewportRequested,
        this,
        &Editor_Main_Window::toggle_viewport);
    connect(
        neovim_,
        &Editor_Neovim_Adapter::playRequested,
        this,
        [this]()
        {
            focus_viewport();
            neovim_->notify_user(
                QStringLiteral("viewport navigation capture is active; press Escape or run :Kalam stop to return to Neovim"),
                false);
        });
    connect(
        neovim_,
        &Editor_Neovim_Adapter::stopRequested,
        this,
        [this]()
        {
            viewport_->cancel_active_gizmo();
            neovim_->focus_editor();
        });
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
            if (context_write_pending_)
            {
                context_write_error_ = error;
            }
            if (generated_form_write_pending_)
            {
                generated_form_write_error_ = error;
            }
            neovim_->notify_user(error, true);
        });
    connect(
        transactions_,
        &Author_Transaction_Controller::transactionClosed,
        this,
        [this](bool accepted)
        {
            if (shutting_down_)
            {
                clear_context_popup();
                return;
            }

            if (generated_form_write_pending_)
            {
                if (!accepted)
                {
                    fail_generated_form_write(
                        generated_form_write_error_.isEmpty()
                            ? QStringLiteral("form write failed")
                            : generated_form_write_error_);
                    return;
                }

                generated_form_transaction_accepted_ = true;
                maybe_finish_generated_form_write_step();
                return;
            }

            if (context_popup_ == nullptr || !context_popup_open_)
                return;

            if (context_write_pending_)
            {
                const bool close_after_write = context_close_after_write_;
                if (accepted && close_after_write)
                {
                    clear_context_popup();
                    neovim_->focus_editor();
                    return;
                }

                if (accepted)
                {
                    begin_context_rearm(false);
                }
                else
                {
                    QTimer::singleShot(
                        0,
                        this,
                        [this]()
                        {
                            if (context_popup_ != nullptr &&
                                context_popup_open_)
                            {
                                begin_context_rearm(true);
                            }
                            else
                            {
                                clear_context_popup();
                            }
                        });
                }
                return;
            }

            clear_context_popup();
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
                const Document_Handle accepted =
                    runtime_host_->document_for_buffer(snapshot.buffer);
                const QString initial_path = QFileInfo(
                    configuration_.initial_file).absoluteFilePath();
                const QString snapshot_path =
                    QFileInfo(snapshot.path).absoluteFilePath();
                if (!initial_path.isEmpty() &&
                    initial_path.compare(
                        snapshot_path,
                        Qt::CaseInsensitive) == 0)
                {
                    initial_document_ = accepted;
                }

                const bool generated_write_snapshot =
                    generated_form_write_pending_ &&
                    generated_form_ != nullptr &&
                    snapshot.buffer == generated_form_->document().buffer &&
                    snapshot.changedtick > generated_form_write_base_tick_;
                if (generated_write_snapshot)
                {
                    generated_form_document_refreshed_ = true;
                    maybe_finish_generated_form_write_step();
                }
                else
                {
                    transactions_->document_changed(snapshot);
                    refresh_generated_form();
                }
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
        &Editor_Neovim_Adapter::contextForPathFinished,
        this,
        &Editor_Main_Window::handle_context_rearm);
    connect(
        neovim_,
        &Editor_Neovim_Adapter::contextForPathFinished,
        this,
        &Editor_Main_Window::handle_smoke_context);
    connect(
        viewport_,
        &Engine_Viewport_Widget::returnToEditorRequested,
        neovim_,
        &Editor_Neovim_Adapter::focus_editor);
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
    if (context_rearming_ && context_popup_ != nullptr)
    {
        const QVector<Author_Value_Owned> staged_values =
            context_popup_->staged_values();
        context_fields_ = fields;
        context_source_ = context;
        context_popup_->set_values(values, true);

        const bool failed_write = context_rearm_failed_write_;
        QString message = failed_write
            ? (context_write_error_.isEmpty()
                ? QStringLiteral("context write failed; staged edits were preserved")
                : context_write_error_)
            : QString();

        if (failed_write && staged_values.size() == fields.size())
        {
            QString preview_error;
            bool restored = true;
            for (qsizetype index = 0; index < fields.size(); ++index)
            {
                if (!transactions_->preview_field(
                        fields.at(index).id,
                        staged_values.at(index),
                        &preview_error))
                {
                    restored = false;
                    break;
                }
            }
            if (restored)
            {
                context_popup_->set_values(staged_values, false);
            }
            else
            {
                message += QStringLiteral(
                    "; the refreshed schema could not restore every staged value");
                if (!preview_error.isEmpty())
                    message += QStringLiteral(": ") + preview_error;
            }
        }

        context_write_pending_ = false;
        context_close_after_write_ = false;
        context_rearming_ = false;
        context_rearm_failed_write_ = false;
        context_write_error_.clear();
        context_popup_->set_busy(false);
        context_popup_->set_validation_error(message);
        project_context_popup();
        context_popup_->focus_modal();
        return;
    }

    clear_context_popup();
    const Cursor_Geometry geometry = shell_->cursor_geometry();
    if (context.screen_row >= 0 &&
        context.screen_column >= 0 &&
        geometry.cell_size.width() > 0 &&
        geometry.cell_size.height() > 0)
    {
        context_anchor_rect_ = QRect(
            static_cast<int>(context.screen_column) *
                geometry.cell_size.width(),
            static_cast<int>(context.screen_row) *
                geometry.cell_size.height(),
            geometry.cell_size.width(),
            geometry.cell_size.height());
    }
    else
    {
        context_anchor_rect_ = geometry.cursor_rect;
    }
    context_fields_ = fields;
    context_source_ = context;
    context_popup_open_ = true;
    context_popup_ = new Context_Popup(fields, values, shell_);
    context_popup_->hide();

    connect(
        context_popup_,
        &Context_Popup::fieldPreviewRequested,
        this,
        [this](Author_Field_Id field_id, const Author_Value_Owned& value)
        {
            if (context_popup_ == nullptr ||
                context_write_pending_ ||
                !transactions_->is_active())
            {
                return;
            }

            QString error;
            if (!transactions_->preview_field(field_id, value, &error))
                context_popup_->set_validation_error(error);
            else
                context_popup_->set_validation_error(QString());
        });
    connect(
        context_popup_,
        &Context_Popup::applyRequested,
        this,
        [this](bool close_after_write)
        {
            if (context_popup_ == nullptr ||
                context_write_pending_ ||
                !transactions_->is_active())
            {
                return;
            }

            context_close_after_write_ = close_after_write;
            context_write_pending_ = true;
            context_write_error_.clear();
            context_popup_->set_busy(true);
            transactions_->accept();
        });
    connect(
        context_popup_,
        &Context_Popup::cancelRequested,
        this,
        [this]()
        {
            if (context_write_pending_)
                return;
            if (transactions_->is_active())
            {
                transactions_->cancel();
                return;
            }
            clear_context_popup();
            neovim_->focus_editor();
        });

    project_context_popup();
    context_popup_->focus_modal();
}

void Editor_Main_Window::begin_context_rearm(bool failed_write)
{
    if (context_popup_ == nullptr ||
        !context_popup_open_ ||
        context_rearm_request_id_ != 0)
    {
        return;
    }
    context_rearming_ = true;
    context_rearm_failed_write_ = failed_write;
    context_rearm_request_id_ = neovim_->request_context_for_path(
        context_source_.buffer,
        context_source_.json_path);
}

void Editor_Main_Window::handle_context_rearm(
    quint64 request_id,
    bool succeeded,
    const Source_Context& context,
    const QString& error)
{
    if (request_id == 0 || request_id != context_rearm_request_id_)
        return;

    context_rearm_request_id_ = 0;
    if (context_popup_ == nullptr || !context_popup_open_)
    {
        clear_context_popup();
        return;
    }
    if (!succeeded)
    {
        const QString message = error.isEmpty()
            ? QStringLiteral("could not refresh the context after writing")
            : error;
        context_write_pending_ = false;
        context_close_after_write_ = false;
        context_rearming_ = false;
        context_rearm_failed_write_ = false;
        context_write_error_.clear();
        context_popup_->set_busy(false);
        context_popup_->set_validation_error(message);
        context_popup_->focus_modal();
        return;
    }

    QString begin_error;
    if (!transactions_->begin_context_edit(context, &begin_error))
    {
        context_write_pending_ = false;
        context_close_after_write_ = false;
        context_rearming_ = false;
        context_rearm_failed_write_ = false;
        context_write_error_.clear();
        context_popup_->set_busy(false);
        context_popup_->set_validation_error(begin_error);
        context_popup_->focus_modal();
    }
}

void Editor_Main_Window::project_context_popup()
{
    if (context_popup_ == nullptr || !context_popup_open_)
        return;

    context_popup_->project_to(
        shell_,
        context_anchor_rect_,
        shell_->cursor_geometry().cell_size);
}

void Editor_Main_Window::clear_context_popup()
{
    if (context_popup_ != nullptr)
    {
        context_popup_->hide();
        context_popup_->deleteLater();
        context_popup_ = nullptr;
    }
    context_fields_.clear();
    context_source_ = {};
    context_rearm_request_id_ = 0;
    context_popup_open_ = false;
    context_write_pending_ = false;
    context_close_after_write_ = false;
    context_rearming_ = false;
    context_rearm_failed_write_ = false;
    context_write_error_.clear();
    context_anchor_rect_ = {};
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

    if (transactions_->is_active())
        transactions_->cancel();
    if (context_popup_ != nullptr)
        clear_context_popup();

    reset_generated_form_write();
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
            Q_UNUSED(original);
            QString error;
            if (!runtime_host_->preview_author_field(
                    document.document_id,
                    document.root_object_id,
                    field.id,
                    value,
                    &error))
            {
                if (generated_form_ != nullptr)
                    generated_form_->set_validation_error(error);
                neovim_->notify_user(error, true);
            }
            else if (generated_form_ != nullptr)
            {
                generated_form_->set_validation_error(QString());
            }
        });
    connect(
        generated_form_,
        &Generated_Form::writeRequested,
        this,
        [this](bool close_after_write)
        {
            begin_generated_form_write(close_after_write);
        });
    const QPointer<Generated_Form> form = generated_form_;
    connect(
        generated_form_,
        &Generated_Form::cancelAllRequested,
        this,
        [this, form]()
        {
            if (generated_form_ == form)
                cancel_generated_form();
            QTimer::singleShot(
                0,
                this,
                [this, form]()
                {
                    if (form == nullptr || !form->isVisible())
                    {
                        neovim_->focus_editor();
                    }
                });
        });
    generated_form_->show();
    generated_form_->raise();
    generated_form_->activateWindow();
    generated_form_->focus_modal();
}

void Editor_Main_Window::refresh_generated_form(
    Author_Field_Id committed_field)
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
    generated_form_->refresh(document, values, committed_field);
    generated_form_->replay_previews();
}

void Editor_Main_Window::begin_generated_form_write(bool close_after_write)
{
    if (generated_form_ == nullptr || generated_form_write_pending_)
        return;

    generated_form_write_pending_ = true;
    generated_form_close_after_write_ = close_after_write;
    generated_form_write_error_.clear();
    generated_form_->set_validation_error(QString());
    generated_form_->set_busy(true);
    continue_generated_form_write();
}

void Editor_Main_Window::continue_generated_form_write()
{
    if (generated_form_ == nullptr || !generated_form_write_pending_)
    {
        reset_generated_form_write();
        return;
    }

    Field_Descriptor field;
    Author_Value_Owned original;
    Author_Value_Owned value;
    if (!generated_form_->next_dirty_edit(&field, &original, &value))
    {
        const bool close_after_write = generated_form_close_after_write_;
        reset_generated_form_write();
        generated_form_->set_busy(false);
        generated_form_->set_validation_error(QString());
        if (close_after_write)
        {
            generated_form_->close_committed();
            neovim_->focus_editor();
        }
        else
        {
            generated_form_->focus_modal();
        }
        return;
    }

    generated_form_write_field_ = field.id;
    generated_form_write_base_tick_ =
        generated_form_->document().changedtick;
    generated_form_transaction_accepted_ = false;
    generated_form_document_refreshed_ = false;
    generated_form_write_error_.clear();

    QString error;
    if (!transactions_->begin_form_field_edit(
            generated_form_->document(),
            field,
            original,
            &error) ||
        !transactions_->preview_field(field.id, value, &error))
    {
        fail_generated_form_write(error);
        return;
    }
    transactions_->accept();
}

void Editor_Main_Window::maybe_finish_generated_form_write_step()
{
    if (!generated_form_write_pending_ ||
        !generated_form_transaction_accepted_ ||
        !generated_form_document_refreshed_)
    {
        return;
    }

    const Author_Field_Id committed_field = generated_form_write_field_;
    generated_form_transaction_accepted_ = false;
    generated_form_document_refreshed_ = false;
    generated_form_write_field_ = 0;
    generated_form_write_base_tick_ = 0;

    refresh_generated_form(committed_field);
    if (generated_form_ == nullptr)
    {
        reset_generated_form_write();
        return;
    }
    continue_generated_form_write();
}

void Editor_Main_Window::fail_generated_form_write(const QString& error)
{
    const QPointer<Generated_Form> form = generated_form_;
    QString message = error.isEmpty()
        ? QStringLiteral("form write failed")
        : error;
    const Author_Document_Id document_id =
        form == nullptr ? 0 : form->document().document_id;

    reset_generated_form_write();
    if (transactions_->is_active())
        transactions_->cancel();

    if (document_id != 0)
    {
        QString rehydrate_error;
        if (!runtime_host_->rehydrate_document(document_id, &rehydrate_error) &&
            !rehydrate_error.isEmpty())
        {
            message += QStringLiteral("; ") + rehydrate_error;
        }
    }

    if (form != nullptr)
    {
        form->set_busy(false);
        form->set_validation_error(message);
        form->replay_previews();
        form->focus_modal();
    }
}

void Editor_Main_Window::reset_generated_form_write()
{
    generated_form_write_pending_ = false;
    generated_form_close_after_write_ = false;
    generated_form_transaction_accepted_ = false;
    generated_form_document_refreshed_ = false;
    generated_form_write_field_ = 0;
    generated_form_write_base_tick_ = 0;
    generated_form_write_error_.clear();
}

void Editor_Main_Window::cancel_generated_form()
{
    if (generated_form_write_pending_)
        return;

    if (transactions_->is_active())
        transactions_->cancel();

    if (generated_form_ != nullptr)
    {
        QString error;
        if (!runtime_host_->rehydrate_document(
                generated_form_->document().document_id,
                &error) &&
            !error.isEmpty())
        {
            neovim_->notify_user(error, true);
        }
    }
    reset_generated_form_write();
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

void Editor_Main_Window::update_viewport_overlay()
{
    const QRect cells =
        shell_->model().window_screen_rect(active_viewport_window_);
    const bool floating =
        shell_->model().window_is_floating(active_viewport_window_);
    if (!cells.isValid() || floating ||
        cells.width() < 8 || cells.height() < 3)
    {
        viewport_->hide();
        return;
    }
    const QSize cell = shell_->cell_size();
    viewport_->setGeometry(
        cells.left() * cell.width(),
        cells.top() * cell.height(),
        cells.width() * cell.width(),
        cells.height() * cell.height());
    viewport_->show();
    viewport_->raise();
}

void Editor_Main_Window::focus_viewport()
{
    update_viewport_overlay();
    if (viewport_->isVisible())
    {
        viewport_->setFocus(Qt::OtherFocusReason);
    }
}

void Editor_Main_Window::toggle_viewport()
{
    /*
     * Layout is Neovim's: :kalam toggle viewport opens or closes the
     * kalam://viewport window in Lua and the overlay follows win_pos.
     * This slot only re-syncs the overlay.
     */
    update_viewport_overlay();
    if (!viewport_->isVisible())
    {
        neovim_->focus_editor();
    }
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
    int timeout_milliseconds,
    const QString& screenshot_path)
{
    if (smoke_timer_.isActive())
    {
        finish_smoke(false, QStringLiteral("acceptance smoke already active"));
        return;
    }
    smoke_cube_document_ = QFileInfo(cube_document).absoluteFilePath();
    smoke_post_document_ = QFileInfo(post_document).absoluteFilePath();
    smoke_screenshot_path_ = screenshot_path.isEmpty()
        ? QString()
        : QFileInfo(screenshot_path).absoluteFilePath();
    smoke_timeout_milliseconds_ = qMax(1000, timeout_milliseconds);
    smoke_documents_applied_ = false;
    smoke_initial_document_verified_ = false;
    smoke_frame_floor_ = 0;
    smoke_context_request_id_ = 0;
    smoke_context_phase_ = 0;
    smoke_context_initial_field_ = 0;
        smoke_clock_.start();
    smoke_timer_.start();
    QTimer::singleShot(
        0,
        this,
        &Editor_Main_Window::run_smoke_step);
}

void Editor_Main_Window::handle_smoke_context(
    quint64 request_id,
    bool succeeded,
    const Source_Context& context,
    const QString& error)
{
    if (request_id == 0 || request_id != smoke_context_request_id_)
    {
        return;
    }
    smoke_context_request_id_ = 0;
    if (!succeeded)
    {
        finish_smoke(
            false,
            error.isEmpty()
                ? QStringLiteral("context acceptance lookup failed")
                : error);
        return;
    }
    QString begin_error;
    if (!transactions_->begin_context_edit(context, &begin_error))
    {
        finish_smoke(false, begin_error);
        return;
    }
    smoke_context_phase_ = 2;
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
            QStringLiteral(
                "timed out waiting for the integrated editor "
                "(context phase %1, mode %2, field %3, dirty %4, "
                "validation %5, viewport windows %6 active %7 rect %8 "
                "visible %9 initialized %10 grids %11)")
                .arg(smoke_context_phase_)
                .arg(context_popup_ != nullptr
                    ? QString::fromUtf8(context_popup_->mode())
                    : QStringLiteral("closed"))
                .arg(context_popup_ != nullptr
                    ? context_popup_->active_field()
                    : -1)
                .arg(context_popup_ != nullptr &&
                    context_popup_->is_dirty() ? 1 : 0)
                .arg(context_popup_ != nullptr &&
                    context_popup_->has_validation_error() ? 1 : 0)
                .arg(viewport_windows_.size())
                .arg(active_viewport_window_)
                .arg(QStringLiteral("%1,%2 %3x%4")
                    .arg(shell_->model()
                        .window_screen_rect(active_viewport_window_).left())
                    .arg(shell_->model()
                        .window_screen_rect(active_viewport_window_).top())
                    .arg(shell_->model()
                        .window_screen_rect(active_viewport_window_).width())
                    .arg(shell_->model()
                        .window_screen_rect(active_viewport_window_).height()))
                .arg(viewport_->isVisible() ? 1 : 0)
                .arg(runtime_host_->viewport_initialized() ? 1 : 0)
                .arg(shell_->model().rows() > 0 ? 1 : 0));
        return;
    }

    if (centralWidget() != shell_ || viewport_->parentWidget() != shell_)
    {
        finish_smoke(
            false,
            QStringLiteral(
                "editor layout is not the buffer-hosted viewport overlay"));
        return;
    }
    if (viewport_->isVisible())
    {
        const QRect window_cells =
            shell_->model().window_screen_rect(active_viewport_window_);
        const QSize cell = shell_->cell_size();
        const QRect expected(
            window_cells.left() * cell.width(),
            window_cells.top() * cell.height(),
            window_cells.width() * cell.width(),
            window_cells.height() * cell.height());
        if (!window_cells.isValid() ||
            (viewport_->geometry().topLeft() - expected.topLeft())
                    .manhattanLength() > cell.width() + cell.height() ||
            qAbs(viewport_->geometry().width() - expected.width()) >
                cell.width() ||
            qAbs(viewport_->geometry().height() - expected.height()) >
                cell.height())
        {
            finish_smoke(
                false,
                QStringLiteral("the viewport overlay does not track its "
                               "Neovim window"));
            return;
        }
    }

    if (!neovim_->is_ready() ||
        !shell_->isNeovimAttached() ||
        !runtime_host_->viewport_initialized() ||
        !runtime_host_->has_callable_module() ||
        runtime_host_->active_module_generation() == 0 ||
        runtime_host_->schemas().size() != 2 ||
        !initial_document_.is_valid() ||
        runtime_host_->rendered_frame_count() < 2 ||
        viewport_->physical_size().isEmpty())
    {
        return;
    }

    if (!smoke_initial_document_verified_)
    {
        Schema_Descriptor initial_schema;
        if (!runtime_host_->schema_for_document(
                initial_document_.document_id,
                &initial_schema) ||
            initial_schema.schema_id != QByteArrayLiteral("nveditor.scene/v1"))
        {
            finish_smoke(
                false,
                QStringLiteral(
                    "the initial positional scene was not registered"));
            return;
        }

        bool found_transform = false;
        Field_Descriptor transform_field;
        for (const Field_Descriptor& field : initial_schema.fields)
        {
            if (field.json_path == QByteArrayLiteral("$.cube.transform"))
            {
                transform_field = field;
                found_transform = true;
                break;
            }
        }
        Author_Value_Owned initial_transform;
        QString error;
        if (!found_transform ||
            !runtime_host_->read_author_field(
                initial_document_.document_id,
                initial_document_.root_object_id,
                transform_field.id,
                &initial_transform,
                &error))
        {
            finish_smoke(
                false,
                error.isEmpty()
                    ? QStringLiteral(
                        "the initial positional scene did not reach the module")
                    : error);
            return;
        }
        smoke_initial_document_verified_ = true;
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

    if (smoke_context_phase_ == 0)
    {
        smoke_context_request_id_ = neovim_->request_context_for_path(
            initial_document_.buffer,
            QStringLiteral("$.cube"));
        smoke_context_phase_ = 1;
        return;
    }
    if (smoke_context_phase_ == 1)
    {
        return;
    }
    auto send_popup_key = [](
        QWidget* target,
        int key,
        const QString& text = QString(),
        Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        if (target == nullptr)
            return;
        QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
        QApplication::sendEvent(target, &event);
    };

    if (smoke_context_phase_ == 2)
    {
        if (!context_popup_open_ || context_popup_ == nullptr ||
            !context_popup_->isVisible())
        {
            return;
        }

        const QSize cell = shell_->cursor_geometry().cell_size;
        QWidget* focused = QApplication::focusWidget();
        QLabel* statusline = context_popup_->findChild<QLabel*>(
            QStringLiteral("kalamContextStatusline"));
        if (context_fields_.size() < 2 ||
            context_popup_->mode() != QByteArrayLiteral("n") ||
            focused != context_popup_ ||
            !context_popup_->has_modal_selection() ||
            cell.height() <= 0 ||
            context_popup_->width() > 520 ||
            context_popup_->height() > 420 ||
            shell_->height() - context_popup_->geometry().bottom() <
                2 * cell.height() ||
            statusline == nullptr ||
            statusline->geometry().bottom() < context_popup_->height() - 4)
        {
            finish_smoke(
                false,
                QStringLiteral(
                    "Qt context did not own Normal-mode focus, graphical selection, compact placement, or its bottom statusline"));
            return;
        }

        smoke_context_initial_field_ = context_popup_->active_field();
        send_popup_key(context_popup_, Qt::Key_J, QStringLiteral("j"));
        smoke_context_phase_ = 3;
        return;
    }
    if (smoke_context_phase_ == 3)
    {
        if (context_popup_ == nullptr ||
            context_popup_->active_field() <= smoke_context_initial_field_)
        {
            finish_smoke(
                false,
                QStringLiteral("Qt context j did not move to the next field"));
            return;
        }
        send_popup_key(context_popup_, Qt::Key_I, QStringLiteral("i"));
        smoke_context_phase_ = 4;
        return;
    }
    if (smoke_context_phase_ == 4)
    {
        if (context_popup_ == nullptr)
        {
            finish_smoke(false, QStringLiteral("Qt context closed during Insert entry"));
            return;
        }

        QWidget* focused = QApplication::focusWidget();
        if (context_popup_->mode() != QByteArrayLiteral("i") ||
            focused == nullptr ||
            focused == context_popup_ ||
            !context_popup_->isAncestorOf(focused))
        {
            finish_smoke(
                false,
                QStringLiteral("Qt context i did not focus the selected field in Insert mode"));
            return;
        }

        for (int index = 0; index < 5; ++index)
            send_popup_key(focused, Qt::Key_I, QStringLiteral("i"));
        if (context_popup_->mode() != QByteArrayLiteral("i") ||
            QApplication::focusWidget() == nullptr ||
            !context_popup_->isAncestorOf(QApplication::focusWidget()))
        {
            finish_smoke(
                false,
                QStringLiteral("Insert-mode keystrokes escaped from the Qt context"));
            return;
        }

        QDoubleSpinBox* selected_spin = nullptr;
        for (QWidget* candidate = QApplication::focusWidget();
             candidate != nullptr && candidate != context_popup_;
             candidate = candidate->parentWidget())
        {
            selected_spin = qobject_cast<QDoubleSpinBox*>(candidate);
            if (selected_spin != nullptr)
                break;
        }
        if (selected_spin == nullptr)
        {
            finish_smoke(
                false,
                QStringLiteral("Qt context Insert target was not the selected numeric field"));
            return;
        }
        selected_spin->setValue(
            selected_spin->value() + selected_spin->singleStep());
        smoke_context_phase_ = 5;
        return;
    }
    if (smoke_context_phase_ == 5)
    {
        if (context_popup_ == nullptr || !context_popup_->is_dirty())
            return;

        const Document_Handle staged_document =
            runtime_host_->document_for_buffer(context_source_.buffer);
        if (!transactions_->is_active() ||
            !staged_document.is_valid() ||
            staged_document.changedtick != context_source_.changedtick)
        {
            finish_smoke(
                false,
                QStringLiteral(
                    "Qt staging changed the source document before an explicit write"));
            return;
        }

        send_popup_key(QApplication::focusWidget(), Qt::Key_Escape);
        smoke_context_phase_ = 6;
        return;
    }
    if (smoke_context_phase_ == 6)
    {
        if (context_popup_ == nullptr ||
            context_popup_->mode() != QByteArrayLiteral("n") ||
            QApplication::focusWidget() != context_popup_)
        {
            finish_smoke(
                false,
                QStringLiteral("Escape did not return Qt context focus to Normal mode"));
            return;
        }

        if (!smoke_screenshot_path_.isEmpty() &&
            !grab().save(smoke_screenshot_path_))
        {
            finish_smoke(false, QStringLiteral("could not save context smoke screenshot"));
            return;
        }

        send_popup_key(context_popup_, Qt::Key_Colon, QStringLiteral(":"));
        send_popup_key(context_popup_, Qt::Key_Q, QStringLiteral("q"));
        send_popup_key(context_popup_, Qt::Key_Exclam, QStringLiteral("!"));
        send_popup_key(context_popup_, Qt::Key_Return);
        smoke_context_phase_ = 7;
        return;
    }
    if (smoke_context_phase_ == 7)
    {
        if (context_popup_ != nullptr || transactions_->is_active())
            return;
        finish_smoke(
            true,
            QStringLiteral(
                "integrated Neovim, WARP viewport, documents, selection, preview rollback, rendering, and Qt-owned modal context staging passed"));
    }
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
