#include "editor_runtime_host.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <cstring>
namespace Kalam::Editor
{

Author_Document_State Editor_Runtime_Host::Owned_Document::borrow()
{
    static const char root_path[] = "$";
    object_source.object_id = handle.root_object_id;
    object_source.source_id = {};
    object_source.json_path.data = root_path;
    object_source.json_path.size = 1;

    Author_Document_State state = {};
    state.struct_size = sizeof(state);
    state.document_id = handle.document_id;
    state.root_object_id = handle.root_object_id;
    state.revision = handle.revision;
    state.schema_id.data = handle.schema_id.constData();
    state.schema_id.size = static_cast<uint64_t>(handle.schema_id.size());
    state.project_relative_path.data = project_relative_path.constData();
    state.project_relative_path.size =
        static_cast<uint64_t>(project_relative_path.size());
    state.json_utf8.data = json_utf8.constData();
    state.json_utf8.size = static_cast<uint64_t>(json_utf8.size());
    state.objects = &object_source;
    state.object_count = 1;
    return state;
}

Editor_Runtime_Host::Editor_Runtime_Host(QObject* parent)
    : QObject(parent)
{
    frame_scheduler_init(&scheduler_);
    input_latch_init(&input_);
    initialize_loader_block(&loader_blocks_[0]);
    initialize_loader_block(&loader_blocks_[1]);
    active_ = &loader_blocks_[0].slot;
    candidate_ = &loader_blocks_[1].slot;

    frame_timer_.setTimerType(Qt::PreciseTimer);
    frame_timer_.setInterval(16);
    connect(
        &frame_timer_,
        &QTimer::timeout,
        this,
        &Editor_Runtime_Host::present_frame);
}

Editor_Runtime_Host::~Editor_Runtime_Host()
{
    shutdown();
}

void Editor_Runtime_Host::set_project_root(const QString& project_root)
{
    project_root_ = QFileInfo(project_root).absoluteFilePath();
}

void Editor_Runtime_Host::set_force_warp(bool force_warp)
{
    Q_ASSERT(viewport_.initialized == 0);
    force_warp_ = force_warp;
}

void Editor_Runtime_Host::set_acceptance_smoke_mode(bool enabled)
{
    Q_ASSERT(viewport_.initialized == 0);
    acceptance_smoke_mode_ = enabled;
}

bool Editor_Runtime_Host::initialize_viewport(
    void* native_window,
    const QSize& physical_size,
    bool enable_debug_layer,
    QString* error)
{
    if (viewport_.initialized != 0 || native_window == nullptr)
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("invalid or repeated viewport initialization");
        }
        return false;
    }

    D3D11_Viewport_Create_Desc desc = {};
    desc.struct_size = sizeof(desc);
    desc.flags = enable_debug_layer
        ? D3D11_Viewport_Create_Enable_Debug_Layer
        : 0;
    desc.window_handle = native_window;
    desc.width = static_cast<uint32_t>(qMax(0, physical_size.width()));
    desc.height = static_cast<uint32_t>(qMax(0, physical_size.height()));
    desc.driver = force_warp_
        ? D3D11_Viewport_Driver_Warp
        : D3D11_Viewport_Driver_Hardware;

    const D3D11_Viewport_Result result =
        d3d11_viewport_host_create(&viewport_, &desc);
    if (result != D3D11_Viewport_Result_Ok)
    {
        if (error != nullptr)
        {
            *error = QString::fromLatin1(d3d11_viewport_result_string(result));
        }
        return false;
    }

    frame_scheduler_reset_backlog(&scheduler_);
    frame_clock_.start();
    return true;
}

void Editor_Runtime_Host::resize_viewport(const QSize& physical_size)
{
    if (viewport_.initialized == 0)
    {
        return;
    }
    const D3D11_Viewport_Result result = d3d11_viewport_host_resize(
        &viewport_,
        static_cast<uint32_t>(qMax(0, physical_size.width())),
        static_cast<uint32_t>(qMax(0, physical_size.height())));
    if (result != D3D11_Viewport_Result_Ok &&
        result != D3D11_Viewport_Result_Skipped)
    {
        emit viewportDeviceLost(
            QString::fromLatin1(d3d11_viewport_result_string(result)));
    }
    frame_scheduler_reset_backlog(&scheduler_);
}

void Editor_Runtime_Host::set_viewport_visible(bool visible)
{
    viewport_visible_ = visible;
    if (!visible)
    {
        frame_scheduler_reset_backlog(&scheduler_);
    }
}

void Editor_Runtime_Host::start_frames()
{
    if (!shutting_down_ && !frame_timer_.isActive())
    {
        frame_clock_.restart();
        frame_timer_.start();
    }
}

void Editor_Runtime_Host::stop_frames()
{
    frame_timer_.stop();
    frame_scheduler_reset_backlog(&scheduler_);
}

bool Editor_Runtime_Host::queue_module_reload(
    quint64 build_generation,
    const QString& canonical_dll,
    const QString& hot_copy_directory,
    QString* error)
{
    if (shutting_down_ || build_generation == 0 || canonical_dll.isEmpty())
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("invalid module reload request");
        }
        return false;
    }
    if (build_generation <= newest_reload_generation_)
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("stale module build generation");
        }
        return false;
    }

    QDir hot_directory(hot_copy_directory);
    if (!hot_directory.mkpath(QStringLiteral(".")))
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("could not create the hot-module directory");
        }
        return false;
    }

    newest_reload_generation_ = build_generation;
    pending_reload_.generation = build_generation;
    pending_reload_.canonical_dll = QFileInfo(canonical_dll).absoluteFilePath();
    const QString unique_suffix =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    pending_reload_.hot_copy_dll = hot_directory.absoluteFilePath(
        QStringLiteral("game_module_hot_%1_%2.dll")
            .arg(build_generation, 16, 10, QLatin1Char('0'))
            .arg(unique_suffix));
    pending_reload_.pending = true;
    return true;
}

bool Editor_Runtime_Host::accept_valid_document(
    const Buffer_Snapshot& snapshot,
    QString* error)
{
    QJsonParseError parse_error;
    const QJsonDocument parsed =
        QJsonDocument::fromJson(snapshot.text_utf8, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !parsed.isObject())
    {
        const QString message = QStringLiteral("JSON validation mismatch: %1")
            .arg(parse_error.errorString());
        if (error != nullptr)
        {
            *error = message;
        }
        emit documentRejected(snapshot.buffer, snapshot.changedtick, message);
        return false;
    }

    const QString schema_string =
        parsed.object().value(QStringLiteral("$schema")).toString();
    if (schema_string.isEmpty())
    {
        const QString message = QStringLiteral("document has no string $schema");
        if (error != nullptr)
        {
            *error = message;
        }
        emit documentRejected(snapshot.buffer, snapshot.changedtick, message);
        return false;
    }

    const int existing_index = document_index_for_buffer(snapshot.buffer);
    if (existing_index >= 0 &&
        snapshot.changedtick <= documents_.at(existing_index).handle.changedtick)
    {
        return true;
    }

    Owned_Document candidate;
    if (existing_index >= 0)
    {
        candidate.handle = documents_.at(existing_index).handle;
    }
    else
    {
        candidate.handle.document_id = next_document_id_++;
        candidate.handle.root_object_id = next_object_id_++;
    }
    candidate.handle.revision = next_revision_++;
    candidate.handle.buffer = snapshot.buffer;
    candidate.handle.changedtick = snapshot.changedtick;
    candidate.handle.schema_id = schema_string.toUtf8();
    candidate.handle.path = snapshot.path;
    candidate.json_utf8 = snapshot.text_utf8;
    const QString absolute_path = QFileInfo(snapshot.path).absoluteFilePath();
    const QString relative_path = project_root_.isEmpty()
        ? snapshot.path
        : QDir(project_root_).relativeFilePath(absolute_path);
    candidate.project_relative_path = QDir::fromNativeSeparators(relative_path)
        .toUtf8();

    if (has_callable_module())
    {
        Author_Document_State state = candidate.borrow();
        Game_Error game_error = {};
        const Game_Result result = active_->api.apply_document(
            active_->game,
            &state,
            &game_error);
        if (result != Game_Result_Ok)
        {
            const QString message = game_error_text(
                result,
                game_error,
                QStringLiteral("module rejected the authored document"));
            if (error != nullptr)
            {
                *error = message;
            }
            emit documentRejected(snapshot.buffer, snapshot.changedtick, message);
            return false;
        }
    }

    if (existing_index >= 0)
    {
        documents_[existing_index] = candidate;
    }
    else
    {
        documents_.push_back(candidate);
    }
    emit documentAccepted(document_for_buffer(snapshot.buffer));
    return true;
}

bool Editor_Runtime_Host::rehydrate_document(
    Author_Document_Id document_id,
    QString* error)
{
    const int index = document_index_for_id(document_id);
    if (index < 0 || !has_callable_module())
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("document or callable module is unavailable");
        }
        return false;
    }

    Owned_Document candidate = documents_.at(index);
    candidate.handle.revision = next_revision_++;
    Author_Document_State state = candidate.borrow();
    Game_Error game_error = {};
    const Game_Result result =
        active_->api.apply_document(active_->game, &state, &game_error);
    if (result != Game_Result_Ok)
    {
        if (error != nullptr)
        {
            *error = game_error_text(
                result,
                game_error,
                QStringLiteral("could not rehydrate the authored document"));
        }
        return false;
    }
    documents_[index].handle.revision = candidate.handle.revision;
    return true;
}

Document_Handle Editor_Runtime_Host::document_for_buffer(qint64 buffer) const
{
    const int index = document_index_for_buffer(buffer);
    return index >= 0 ? documents_.at(index).handle : Document_Handle{};
}

Document_Handle Editor_Runtime_Host::document_by_id(
    Author_Document_Id document_id) const
{
    const int index = document_index_for_id(document_id);
    return index >= 0 ? documents_.at(index).handle : Document_Handle{};
}

QString Editor_Runtime_Host::source_path_for_object(
    Author_Document_Id document_id,
    Author_Object_Id object_id) const
{
    const int index = document_index_for_id(document_id);
    if (index < 0)
    {
        return {};
    }
    const Owned_Document& document = documents_.at(index);
    if (object_id != document.handle.root_object_id)
    {
        return {};
    }
    if (document.handle.schema_id == QByteArrayLiteral("nveditor.scene/v1"))
    {
        return QStringLiteral("$.cube");
    }
    return QStringLiteral("$");
}

QVector<Schema_Descriptor> Editor_Runtime_Host::schemas() const
{
    QVector<Schema_Descriptor> result;
    if (!has_callable_module())
    {
        return result;
    }

    result.reserve(static_cast<qsizetype>(active_->schema_count));
    for (uint32_t schema_index = 0;
         schema_index < active_->schema_count;
         ++schema_index)
    {
        const Author_Schema_View& source = active_->schemas[schema_index];
        Schema_Descriptor schema;
        schema.schema_id = string_view_copy(source.schema_id);
        schema.schema_version = source.schema_version;
        schema.flags = source.flags;
        schema.fields.reserve(static_cast<qsizetype>(source.field_count));

        for (uint32_t field_index = 0;
             field_index < source.field_count;
             ++field_index)
        {
            const Author_Field_Desc& source_field = source.fields[field_index];
            Field_Descriptor field;
            field.id = source_field.id;
            field.name = string_view_copy(source_field.name);
            field.json_path = string_view_copy(source_field.json_path);
            field.group = string_view_copy(source_field.group);
            field.kind = source_field.kind;
            field.semantic = source_field.semantic;
            field.flags = source_field.flags;
            for (int component = 0; component < 4; ++component)
            {
                field.minimum[component] = source_field.minimum[component];
                field.maximum[component] = source_field.maximum[component];
                field.step[component] = source_field.step[component];
            }
            schema.fields.push_back(field);
        }
        result.push_back(schema);
    }
    return result;
}

bool Editor_Runtime_Host::schema_for_document(
    Author_Document_Id document_id,
    Schema_Descriptor* schema) const
{
    if (schema == nullptr)
    {
        return false;
    }
    const int document_index = document_index_for_id(document_id);
    if (document_index < 0)
    {
        return false;
    }

    const QByteArray wanted = documents_.at(document_index).handle.schema_id;
    const QVector<Schema_Descriptor> available = schemas();
    for (const Schema_Descriptor& item : available)
    {
        if (item.schema_id == wanted)
        {
            *schema = item;
            return true;
        }
    }
    return false;
}

bool Editor_Runtime_Host::read_author_field(
    Author_Document_Id document_id,
    Author_Object_Id object_id,
    Author_Field_Id field_id,
    Author_Value_Owned* value,
    QString* error) const
{
    if (!has_callable_module() || value == nullptr)
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("callable module or output value is unavailable");
        }
        return false;
    }

    Author_Value borrowed_value = {};
    Game_Error game_error = {};
    const Game_Result result = active_->api.read_author_field(
        active_->game,
        document_id,
        object_id,
        field_id,
        &borrowed_value,
        &game_error);
    if (result != Game_Result_Ok)
    {
        if (error != nullptr)
        {
            *error = game_error_text(
                result,
                game_error,
                QStringLiteral("could not read authored field"));
        }
        return false;
    }
    *value = Author_Value_Owned::copy_of(borrowed_value);
    return true;
}

bool Editor_Runtime_Host::preview_author_field(
    Author_Document_Id document_id,
    Author_Object_Id object_id,
    Author_Field_Id field_id,
    const Author_Value_Owned& value,
    QString* error)
{
    if (!has_callable_module())
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("callable module is unavailable");
        }
        return false;
    }

    const Author_Value borrowed_value = value.borrow();
    Game_Error game_error = {};
    const Game_Result result = active_->api.preview_author_field(
        active_->game,
        document_id,
        object_id,
        field_id,
        &borrowed_value,
        &game_error);
    if (result != Game_Result_Ok)
    {
        if (error != nullptr)
        {
            *error = game_error_text(
                result,
                game_error,
                QStringLiteral("could not preview authored field"));
        }
        return false;
    }
    return true;
}

bool Editor_Runtime_Host::viewport_interaction(
    Game_Viewport_Interaction_Kind kind,
    const QPoint& physical_position,
    const QPoint& physical_delta,
    const QSize& physical_viewport_size,
    Viewport_Interaction_Result* result,
    QString* error)
{
    if (result != nullptr)
    {
        *result = {};
    }
    if (!has_callable_module() ||
        active_->api.viewport_interaction == nullptr ||
        result == nullptr ||
        physical_viewport_size.isEmpty())
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("module viewport interaction is unavailable");
        }
        return false;
    }

    Game_Viewport_Interaction interaction = {};
    interaction.struct_size = sizeof(interaction);
    interaction.kind = kind;
    interaction.pointer_x_pixels = physical_position.x();
    interaction.pointer_y_pixels = physical_position.y();
    interaction.pointer_delta_x_pixels = physical_delta.x();
    interaction.pointer_delta_y_pixels = physical_delta.y();
    interaction.viewport_width =
        static_cast<uint32_t>(physical_viewport_size.width());
    interaction.viewport_height =
        static_cast<uint32_t>(physical_viewport_size.height());

    Game_Viewport_Interaction_Result borrowed_result = {};
    Game_Error game_error = {};
    const Game_Result call_result = active_->api.viewport_interaction(
        active_->game,
        &interaction,
        &borrowed_result,
        &game_error);
    if (call_result != Game_Result_Ok)
    {
        if (error != nullptr)
        {
            *error = game_error_text(
                call_result,
                game_error,
                QStringLiteral("module viewport interaction failed"));
        }
        return false;
    }

    result->flags = borrowed_result.flags;
    result->document_id = borrowed_result.document_id;
    result->object_id = borrowed_result.object_id;
    result->field_id = borrowed_result.field_id;
    result->value = Author_Value_Owned::copy_of(borrowed_result.value);
    return true;
}

void Editor_Runtime_Host::set_key(Game_Key key, bool is_down)
{
    input_latch_set_key(&input_, key, is_down ? 1u : 0u);
}

void Editor_Runtime_Host::set_mouse_button(
    Game_Mouse_Button button,
    bool is_down)
{
    input_latch_set_mouse_button(&input_, button, is_down ? 1u : 0u);
}

void Editor_Runtime_Host::set_pointer_position(
    const QPoint& physical_position)
{
    input_latch_set_pointer_position(
        &input_,
        physical_position.x(),
        physical_position.y());
}

void Editor_Runtime_Host::add_pointer_delta(const QPoint& physical_delta)
{
    input_latch_add_pointer_delta(
        &input_,
        physical_delta.x(),
        physical_delta.y());
}

void Editor_Runtime_Host::add_wheel_delta(float delta)
{
    input_latch_add_wheel_delta(&input_, delta);
}

void Editor_Runtime_Host::viewport_focus_gained()
{
    input_latch_focus_gained(&input_);
}

void Editor_Runtime_Host::viewport_focus_lost()
{
    input_latch_focus_lost(&input_);
}

bool Editor_Runtime_Host::has_callable_module() const
{
    return active_ != nullptr && module_loader_slot_is_callable(active_) != 0;
}

bool Editor_Runtime_Host::viewport_initialized() const
{
    return viewport_.initialized != 0;
}

quint64 Editor_Runtime_Host::rendered_frame_count() const
{
    return rendered_frame_count_;
}

quint64 Editor_Runtime_Host::presented_frame_count() const
{
    return presented_frame_count_;
}

quint64 Editor_Runtime_Host::active_module_generation() const
{
    return has_callable_module() ? active_->build_generation : 0;
}

void Editor_Runtime_Host::shutdown()
{
    if (shutting_down_)
    {
        return;
    }
    shutting_down_ = true;
    stop_frames();
    pending_reload_ = {};
    input_latch_focus_lost(&input_);

    Module_Loader_Error loader_error = {};
    if (candidate_ != nullptr &&
        (candidate_->state == Module_Loader_Slot_Prepared ||
         candidate_->state == Module_Loader_Slot_Active))
    {
        module_loader_release(
            candidate_,
            &Editor_Runtime_Host::clear_context_callback,
            this,
            &loader_error);
    }
    if (candidate_ != nullptr &&
        candidate_->state == Module_Loader_Slot_Inactive_Copy)
    {
        module_loader_delete_inactive_copy(candidate_, &loader_error);
    }
    if (active_ != nullptr && module_loader_slot_is_callable(active_) != 0)
    {
        module_loader_release(
            active_,
            &Editor_Runtime_Host::clear_context_callback,
            this,
            &loader_error);
    }
    if (active_ != nullptr &&
        active_->state == Module_Loader_Slot_Inactive_Copy)
    {
        module_loader_delete_inactive_copy(active_, &loader_error);
    }

    d3d11_viewport_host_destroy(&viewport_);
    documents_.clear();
}

void Editor_Runtime_Host::present_frame()
{
    if (shutting_down_ || viewport_.initialized == 0)
    {
        return;
    }

    process_pending_reload();
    if (!viewport_visible_ || viewport_.target_available == 0)
    {
        frame_clock_.restart();
        frame_scheduler_reset_backlog(&scheduler_);
        return;
    }

    if (!acceptance_smoke_mode_ && viewport_.occluded != 0)
    {
        const D3D11_Viewport_Result probe_result =
            d3d11_viewport_host_present(&viewport_, 0);
        if (probe_result == D3D11_Viewport_Result_Skipped)
        {
            frame_clock_.restart();
            frame_scheduler_reset_backlog(&scheduler_);
            return;
        }
        if (probe_result != D3D11_Viewport_Result_Ok)
        {
            stop_frames();
            emit viewportDeviceLost(
                QString::fromLatin1(
                    d3d11_viewport_result_string(probe_result)));
            return;
        }
        frame_clock_.restart();
        frame_scheduler_reset_backlog(&scheduler_);
    }

    const double elapsed_seconds = frame_clock_.isValid()
        ? static_cast<double>(frame_clock_.nsecsElapsed()) / 1000000000.0
        : 0.0;
    frame_clock_.restart();
    const Frame_Schedule schedule =
        frame_scheduler_advance(&scheduler_, elapsed_seconds);

    if (has_callable_module())
    {
        for (uint32_t update_index = 0;
             update_index < schedule.update_count;
             ++update_index)
        {
            Game_Input input = {};
            input_latch_consume(
                &input_,
                schedule.first_fixed_tick_index + update_index,
                &input);
            active_->api.fixed_update(
                active_->game,
                &input,
                schedule.fixed_delta_time);
        }
    }

    Host_Render_Target target = {};
    if (d3d11_viewport_host_get_render_target(&viewport_, &target) == 0)
    {
        frame_scheduler_reset_backlog(&scheduler_);
        return;
    }

    if (has_callable_module())
    {
        Game_Error game_error = {};
        const Game_Result render_result = active_->api.render(
            active_->game,
            &target,
            schedule.interpolation_alpha,
            &game_error);
        if (render_result != Game_Result_Ok)
        {
            emit frameError(game_error_text(
                render_result,
                game_error,
                QStringLiteral("game module render failed")));
        }
        else
        {
            ++rendered_frame_count_;
        }
    }
    else
    {
        static const float placeholder_color[4] =
            {0.025f, 0.035f, 0.055f, 1.0f};
        d3d11_viewport_host_clear(
            &viewport_,
            placeholder_color,
            1.0f,
            0);
    }

    if (acceptance_smoke_mode_)
    {
        return;
    }

    const D3D11_Viewport_Result present_result =
        d3d11_viewport_host_present(&viewport_, 1);
    if (present_result == D3D11_Viewport_Result_Skipped)
    {
        frame_scheduler_reset_backlog(&scheduler_);
    }
    else if (present_result != D3D11_Viewport_Result_Ok)
    {
        stop_frames();
        emit viewportDeviceLost(
            QString::fromLatin1(
                d3d11_viewport_result_string(present_result)));
    }
    else
    {
        ++presented_frame_count_;
    }
}

void Editor_Runtime_Host::initialize_loader_block(Loader_Block* block)
{
    Module_Loader_Storage storage = {};
    storage.schemas = block->schema_storage;
    storage.schema_capacity = schema_capacity_;
    storage.fields = block->field_storage;
    storage.field_capacity = field_capacity_;
    storage.strings = block->string_storage;
    storage.string_capacity = string_capacity_;
    storage.loaded_path = block->path_storage;
    storage.loaded_path_capacity = path_capacity_;

    Module_Loader_Error error = {};
    const Module_Loader_Result result =
        module_loader_slot_init(&block->slot, &storage, &error);
    Q_ASSERT(result == Module_Loader_Result_Ok);
    Q_UNUSED(result);
}

void Editor_Runtime_Host::process_pending_reload()
{
    if (!pending_reload_.pending || viewport_.initialized == 0)
    {
        return;
    }

    const Pending_Reload reload = pending_reload_;
    pending_reload_.pending = false;
    Module_Loader_Error loader_error = {};

    if (candidate_->state == Module_Loader_Slot_Inactive_Copy)
    {
        const Module_Loader_Result delete_result =
            module_loader_delete_inactive_copy(candidate_, &loader_error);
        if (delete_result != Module_Loader_Result_Ok)
        {
            emit moduleReloadFailed(
                reload.generation,
                loader_error_text(loader_error));
            return;
        }
    }
    if (candidate_->state != Module_Loader_Slot_Empty)
    {
        emit moduleReloadFailed(
            reload.generation,
            QStringLiteral("candidate module slot is not empty"));
        return;
    }

    wchar_t canonical_path[path_capacity_] = {};
    wchar_t hot_copy_path[path_capacity_] = {};
    if (reload.canonical_dll.size() >= path_capacity_ ||
        reload.hot_copy_dll.size() >= path_capacity_)
    {
        emit moduleReloadFailed(
            reload.generation,
            QStringLiteral("module DLL path exceeds the Win32 path capacity"));
        return;
    }
    const qsizetype canonical_size =
        reload.canonical_dll.toWCharArray(canonical_path);
    const qsizetype hot_copy_size =
        reload.hot_copy_dll.toWCharArray(hot_copy_path);
    canonical_path[canonical_size] = L'\0';
    hot_copy_path[hot_copy_size] = L'\0';
    QVector<Author_Document_State> borrowed_documents = borrow_documents();

    Module_Loader_Prepare_Request request = {};
    request.struct_size = sizeof(request);
    request.build_generation = reload.generation;
    request.canonical_dll_path = canonical_path;
    request.unique_hot_dll_path = hot_copy_path;
    request.create_desc.struct_size = sizeof(request.create_desc);
    request.create_desc.host_api_version = Host_API_Version;
    request.create_desc.d3d11_device = viewport_.device;
    request.create_desc.d3d11_immediate_context = viewport_.immediate_context;
    request.create_desc.host_user = this;
    request.create_desc.host_log = &Editor_Runtime_Host::log_callback;
    /* Borrowed for the create call only; the module copies nothing. */
    const QByteArray project_root_utf8 = project_root_.toUtf8();
    request.create_desc.project_root.data = project_root_utf8.constData();
    request.create_desc.project_root.size =
        static_cast<uint64_t>(project_root_utf8.size());
    request.documents = borrowed_documents.constData();
    request.document_count =
        static_cast<uint32_t>(borrowed_documents.size());
    request.clear_context_bindings =
        &Editor_Runtime_Host::clear_context_callback;
    request.clear_context_user = this;

    const Module_Loader_Result prepare_result =
        module_loader_prepare(candidate_, &request, &loader_error);
    if (prepare_result != Module_Loader_Result_Ok)
    {
        const QString message = loader_error_text(loader_error);
        if (candidate_->state == Module_Loader_Slot_Inactive_Copy)
        {
            Module_Loader_Error cleanup_error = {};
            module_loader_delete_inactive_copy(candidate_, &cleanup_error);
        }
        emit moduleReloadFailed(reload.generation, message);
        return;
    }

    if (reload.generation != newest_reload_generation_)
    {
        module_loader_release(
            candidate_,
            &Editor_Runtime_Host::clear_context_callback,
            this,
            &loader_error);
        module_loader_delete_inactive_copy(candidate_, &loader_error);
        emit moduleReloadFailed(
            reload.generation,
            QStringLiteral("candidate became stale before publication"));
        return;
    }

    emit aboutToCommitModuleReload();
    d3d11_viewport_host_clear_bindings(&viewport_, 1);
    const Module_Loader_Result commit_result =
        module_loader_commit(active_, candidate_, &loader_error);
    if (commit_result != Module_Loader_Result_Ok)
    {
        emit moduleReloadFailed(
            reload.generation,
            loader_error_text(loader_error));
        return;
    }

    if (candidate_->state == Module_Loader_Slot_Inactive_Copy)
    {
        Module_Loader_Error cleanup_error = {};
        module_loader_delete_inactive_copy(candidate_, &cleanup_error);
    }
    frame_scheduler_reset_backlog(&scheduler_);
    frame_clock_.restart();
    emit schemasChanged();
    emit moduleReloaded(reload.generation);
}

QVector<Author_Document_State> Editor_Runtime_Host::borrow_documents()
{
    QVector<Author_Document_State> result;
    result.reserve(documents_.size());
    for (Owned_Document& document : documents_)
    {
        result.push_back(document.borrow());
    }
    return result;
}

int Editor_Runtime_Host::document_index_for_buffer(qint64 buffer) const
{
    for (qsizetype index = 0; index < documents_.size(); ++index)
    {
        if (documents_.at(index).handle.buffer == buffer)
        {
            return static_cast<int>(index);
        }
    }
    return -1;
}

int Editor_Runtime_Host::document_index_for_id(
    Author_Document_Id document_id) const
{
    for (qsizetype index = 0; index < documents_.size(); ++index)
    {
        if (documents_.at(index).handle.document_id == document_id)
        {
            return static_cast<int>(index);
        }
    }
    return -1;
}

QByteArray Editor_Runtime_Host::string_view_copy(String_View view)
{
    if (view.data == nullptr || view.size == 0)
    {
        return {};
    }
    return QByteArray(view.data, static_cast<qsizetype>(view.size));
}

QString Editor_Runtime_Host::game_error_text(
    Game_Result result,
    const Game_Error& error,
    const QString& fallback)
{
    QString message = QString::fromUtf8(string_view_copy(error.message));
    if (message.isEmpty())
    {
        message = fallback;
    }
    const QString json_path =
        QString::fromUtf8(string_view_copy(error.json_path));
    if (!json_path.isEmpty())
    {
        message += QStringLiteral(" [%1]").arg(json_path);
    }
    return QStringLiteral("%1 (Game_Result %2)").arg(message).arg(result);
}

QString Editor_Runtime_Host::loader_error_text(
    const Module_Loader_Error& error)
{
    QString message = error.message_size != 0
        ? QString::fromUtf8(error.message, error.message_size)
        : QString::fromLatin1(module_loader_result_string(error.result));
    if (error.json_path_size != 0)
    {
        message += QStringLiteral(" [%1]").arg(
            QString::fromUtf8(error.json_path, error.json_path_size));
    }
    if (error.win32_error != 0)
    {
        message += QStringLiteral(" (Win32 %1)").arg(error.win32_error);
    }
    return message;
}

void Editor_Runtime_Host::clear_context_callback(void* user)
{
    auto* host = static_cast<Editor_Runtime_Host*>(user);
    if (host != nullptr && host->viewport_.initialized != 0)
    {
        d3d11_viewport_host_clear_bindings(&host->viewport_, 1);
    }
}

void Editor_Runtime_Host::log_callback(
    void* user,
    uint32_t level,
    String_View message)
{
    auto* host = static_cast<Editor_Runtime_Host*>(user);
    if (host != nullptr && level >= 2)
    {
        emit host->frameError(QString::fromUtf8(string_view_copy(message)));
    }
}

} // namespace Kalam::Editor
