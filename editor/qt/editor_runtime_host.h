#ifndef KALAM_EDITOR_QT_EDITOR_RUNTIME_HOST_H
#define KALAM_EDITOR_QT_EDITOR_RUNTIME_HOST_H

#include "editor_types.h"

#include "d3d11_viewport_host.h"
#include "frame_scheduler.h"
#include "input_latch.h"
#include "module_loader.h"

#include <QElapsedTimer>
#include <QObject>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVector>

namespace Kalam::Editor
{

class Editor_Runtime_Host final : public QObject
{
    Q_OBJECT

public:
    explicit Editor_Runtime_Host(QObject* parent = nullptr);
    ~Editor_Runtime_Host() override;

    void set_project_root(const QString& project_root);
    void set_force_warp(bool force_warp);
    void set_acceptance_smoke_mode(bool enabled);
    bool initialize_viewport(
        void* native_window,
        const QSize& physical_size,
        bool enable_debug_layer,
        QString* error);
    void resize_viewport(const QSize& physical_size);
    void set_viewport_visible(bool visible);
    void start_frames();
    void stop_frames();

    bool queue_module_reload(
        quint64 build_generation,
        const QString& canonical_dll,
        const QString& hot_copy_directory,
        QString* error);

    bool accept_valid_document(
        const Buffer_Snapshot& snapshot,
        QString* error);
    bool rehydrate_document(Author_Document_Id document_id, QString* error);
    Document_Handle document_for_buffer(qint64 buffer) const;
    Document_Handle document_by_id(Author_Document_Id document_id) const;

    /*
     * The project document registry (docs/entity_documents.md section 4).
     * Every authored file under the project root is a live document,
     * whether or not a buffer has it open; an open buffer shadows the disk
     * copy until it closes.
     */
    struct Registry_Diagnostic
    {
        QString path;
        QString message;
    };

    /*
     * Scans the project root for authored JSON documents, applying every
     * one that validates. Returns the count applied; diagnostics for files
     * that failed are appended to out_diagnostics.
     */
    int scan_project_documents(
        QVector<Registry_Diagnostic>* out_diagnostics);

    /* The inverse the selection loop needs: identity back to its file. */
    QString file_path_for_document(Author_Document_Id document_id) const;

    /* Path-derived identity; the same hash family as authored field ids. */
    static Author_Document_Id document_id_for_relative_path(
        const QString& project_relative_path);

    /* Releases a buffer's shadow so the disk copy governs again. */
    void release_buffer_shadow(qint64 buffer);
    QString source_path_for_object(
        Author_Document_Id document_id,
        Author_Object_Id object_id) const;
    QVector<Schema_Descriptor> schemas() const;
    bool schema_for_document(
        Author_Document_Id document_id,
        Schema_Descriptor* schema) const;

    bool read_author_field(
        Author_Document_Id document_id,
        Author_Object_Id object_id,
        Author_Field_Id field_id,
        Author_Value_Owned* value,
        QString* error) const;
    bool preview_author_field(
        Author_Document_Id document_id,
        Author_Object_Id object_id,
        Author_Field_Id field_id,
        const Author_Value_Owned& value,
        QString* error);
    /*
     * Source to selection (docs/entity_documents.md section 6). A zero
     * document_id clears the highlight. Returns false when the loaded
     * module predates Game_API_Version 2, which is not an error: the
     * editor simply shows no highlight.
     */
    bool highlight_object(
        Author_Document_Id document_id,
        Author_Object_Id object_id);

    bool viewport_interaction(
        Game_Viewport_Interaction_Kind kind,
        const QPoint& physical_position,
        const QPoint& physical_delta,
        const QSize& physical_viewport_size,
        Viewport_Interaction_Result* result,
        QString* error);

    void set_key(Game_Key key, bool is_down);
    void set_mouse_button(Game_Mouse_Button button, bool is_down);
    void set_pointer_position(const QPoint& physical_position);
    void add_pointer_delta(const QPoint& physical_delta);
    void add_wheel_delta(float delta);
    void viewport_focus_gained();
    void viewport_focus_lost();

    bool has_callable_module() const;
    bool viewport_initialized() const;
    quint64 rendered_frame_count() const;
    quint64 presented_frame_count() const;
    quint64 active_module_generation() const;
    void shutdown();

signals:
    void aboutToCommitModuleReload();
    void moduleReloaded(quint64 build_generation);
    void moduleReloadFailed(quint64 build_generation, QString error);
    void schemasChanged();
    void documentAccepted(Kalam::Editor::Document_Handle document);
    void documentRejected(qint64 buffer, quint64 changedtick, QString error);
    void frameError(QString error);
    void viewportDeviceLost(QString error);

private slots:
    void present_frame();

private:
    static constexpr quint32 schema_capacity_ = 32;
    static constexpr quint32 field_capacity_ = 256;
    static constexpr quint64 string_capacity_ = 128 * 1024;
    static constexpr quint32 path_capacity_ = 32768;

    struct Loader_Block
    {
        Module_Loader_Slot slot = {};
        Author_Schema_View schema_storage[schema_capacity_] = {};
        Author_Field_Desc field_storage[field_capacity_] = {};
        char string_storage[string_capacity_] = {};
        wchar_t path_storage[path_capacity_] = {};
    };

    struct Owned_Document
    {
        Document_Handle handle;
        QByteArray project_relative_path;
        QByteArray json_utf8;
        Author_Object_Source object_source = {};
        /*
         * An open buffer shadows the file on disk; a scan never overwrites
         * a shadowed document (docs/entity_documents.md section 4).
         */
        bool buffer_shadowed = false;

        Author_Document_State borrow();
    };

    struct Pending_Reload
    {
        quint64 generation = 0;
        QString canonical_dll;
        QString hot_copy_dll;
        bool pending = false;
    };

    D3D11_Viewport_Host viewport_ = {};
    Frame_Scheduler scheduler_ = {};
    Input_Latch input_ = {};
    Loader_Block loader_blocks_[2] = {};
    Module_Loader_Slot* active_ = nullptr;
    Module_Loader_Slot* candidate_ = nullptr;
    QVector<Owned_Document> documents_;
    Pending_Reload pending_reload_;
    QTimer frame_timer_;
    QElapsedTimer frame_clock_;
    QString project_root_;
    quint64 next_document_id_ = 1;
    quint64 next_object_id_ = 1;
    quint64 next_revision_ = 1;
    quint64 newest_reload_generation_ = 0;
    bool viewport_visible_ = false;
    bool force_warp_ = false;
    bool acceptance_smoke_mode_ = false;
    bool shutting_down_ = false;
    quint64 rendered_frame_count_ = 0;
    quint64 presented_frame_count_ = 0;

    void initialize_loader_block(Loader_Block* block);
    void process_pending_reload();
    QVector<Author_Document_State> borrow_documents();
    int document_index_for_buffer(qint64 buffer) const;
    int document_index_for_id(Author_Document_Id document_id) const;
    int document_index_for_relative_path(
        const QByteArray& project_relative_path) const;
    QString relative_path_for(const QString& path) const;
    bool apply_document_text(
        const QString& path,
        const QByteArray& json_utf8,
        qint64 buffer,
        quint64 changedtick,
        bool from_disk,
        QString* error);
    static QByteArray string_view_copy(String_View view);
    static QString game_error_text(
        Game_Result result,
        const Game_Error& error,
        const QString& fallback);
    static QString loader_error_text(const Module_Loader_Error& error);
    static void clear_context_callback(void* user);
    static void log_callback(void* user, uint32_t level, String_View message);
};

} // namespace Kalam::Editor

Q_DECLARE_METATYPE(Kalam::Editor::Document_Handle)

#endif
