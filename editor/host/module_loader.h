#ifndef KALAM_EDITOR_HOST_MODULE_LOADER_H
#define KALAM_EDITOR_HOST_MODULE_LOADER_H

#include <stddef.h>
#include <stdint.h>

#include "game_api.h"

/*
 * The loader never allocates. Each simultaneously loaded module needs its own
 * caller-owned storage. Author_Schema_View and Author_Field_Desc values in a
 * loaded slot point only into these buffers, never into the DLL.
 */
typedef struct Module_Loader_Storage
{
    Author_Schema_View* schemas;
    uint32_t schema_capacity;

    Author_Field_Desc* fields;
    uint32_t field_capacity;

    char* strings;
    uint64_t string_capacity;

    /*
     * UTF-16 code-unit capacity including the terminating NUL. Win32 long
     * paths can require 32768 code units.
     */
    wchar_t* loaded_path;
    uint32_t loaded_path_capacity;
} Module_Loader_Storage;

typedef uint32_t Module_Loader_Slot_State;

enum
{
    Module_Loader_Slot_Empty = 0,
    Module_Loader_Slot_Prepared = 1,
    Module_Loader_Slot_Active = 2,
    Module_Loader_Slot_Inactive_Copy = 3,
    Module_Loader_Slot_Unload_Pending = 4
};

typedef struct Module_Loader_Slot
{
    Module_Loader_Storage storage;

    /*
     * HMODULE is kept opaque here so Windows headers do not leak through the
     * host boundary. It is non-null only while the DLL is loaded.
     */
    void* native_library;
    void* game;
    Game_API api;

    Author_Schema_View* schemas;
    uint32_t schema_count;
    uint32_t field_count;
    uint64_t string_size;

    uint64_t build_generation;
    uint32_t loaded_path_size;
    Module_Loader_Slot_State state;
} Module_Loader_Slot;

typedef void (*Module_Loader_Clear_Context_Function)(void* user);

typedef struct Module_Loader_Prepare_Request
{
    uint32_t struct_size;
    uint32_t flags;
    uint64_t build_generation;

    const wchar_t* canonical_dll_path;
    const wchar_t* unique_hot_dll_path;

    Game_Create_Desc create_desc;

    /*
     * These must be the host's current last-known-valid snapshots. Calls are
     * synchronous and the module may borrow their contents only for each call.
     */
    const Author_Document_State* documents;
    uint32_t document_count;
    uint32_t reserved;

    /*
     * Used only when prepare must roll a candidate back. A successful prepare
     * leaves the candidate loaded. Before commit, the caller cancels editor
     * transactions, reaches a frame boundary, stops old-module calls, and
     * clears immediate-context bindings itself.
     */
    Module_Loader_Clear_Context_Function clear_context_bindings;
    void* clear_context_user;
} Module_Loader_Prepare_Request;

typedef uint32_t Module_Loader_Result;

enum
{
    Module_Loader_Result_Ok = 0,
    Module_Loader_Result_Invalid_Argument,
    Module_Loader_Result_Invalid_State,
    Module_Loader_Result_Storage_Too_Small,
    Module_Loader_Result_Stale_Generation,
    Module_Loader_Result_Path_Resolution_Failed,
    Module_Loader_Result_Hot_Path_Not_Unique,
    Module_Loader_Result_Copy_Failed,
    Module_Loader_Result_Load_Failed,
    Module_Loader_Result_Entry_Point_Missing,
    Module_Loader_Result_Negotiation_Failed,
    Module_Loader_Result_API_Version_Mismatch,
    Module_Loader_Result_API_Size_Mismatch,
    Module_Loader_Result_API_Function_Missing,
    Module_Loader_Result_Create_Failed,
    Module_Loader_Result_Create_Returned_Null,
    Module_Loader_Result_Schema_Query_Failed,
    Module_Loader_Result_Schema_Invalid,
    Module_Loader_Result_Document_Apply_Failed,
    Module_Loader_Result_Unload_Failed,
    Module_Loader_Result_Delete_Failed
};

typedef uint32_t Module_Loader_Stage;

enum
{
    Module_Loader_Stage_None = 0,
    Module_Loader_Stage_Resolve_Path,
    Module_Loader_Stage_Copy,
    Module_Loader_Stage_Load,
    Module_Loader_Stage_Resolve_Entry,
    Module_Loader_Stage_Negotiate_API,
    Module_Loader_Stage_Validate_API,
    Module_Loader_Stage_Create,
    Module_Loader_Stage_Query_Schema_Count,
    Module_Loader_Stage_Query_Schema,
    Module_Loader_Stage_Validate_Schema,
    Module_Loader_Stage_Copy_Schema,
    Module_Loader_Stage_Apply_Document,
    Module_Loader_Stage_Unload,
    Module_Loader_Stage_Delete
};

enum
{
    Module_Loader_Error_Message_Capacity = 1024,
    Module_Loader_Error_JSON_Path_Capacity = 512
};

/*
 * This error owns all text. It never retains Game_Error String_View values
 * across another module call or across FreeLibrary.
 */
typedef struct Module_Loader_Error
{
    Module_Loader_Result result;
    Module_Loader_Stage stage;
    uint32_t win32_error;
    Game_Result game_result;

    uint32_t schema_index;
    uint32_t field_index;
    uint32_t document_index;
    uint32_t message_was_truncated;
    uint32_t json_path_was_truncated;

    uint32_t message_size;
    uint32_t json_path_size;
    char message[Module_Loader_Error_Message_Capacity];
    char json_path[Module_Loader_Error_JSON_Path_Capacity];
} Module_Loader_Error;

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * Initializes an empty slot around caller-owned storage. The storage must
 * outlive every prepared, active, or inactive-copy state held by the slot.
 */
Module_Loader_Result module_loader_slot_init(
    Module_Loader_Slot* slot,
    const Module_Loader_Storage* storage,
    Module_Loader_Error* out_error);

/*
 * Copies and fully validates a candidate without touching the active slot.
 * On success, candidate becomes Prepared. On failure after a successful copy,
 * it becomes Inactive_Copy so deletion remains a separate explicit action.
 */
Module_Loader_Result module_loader_prepare(
    Module_Loader_Slot* candidate,
    const Module_Loader_Prepare_Request* request,
    Module_Loader_Error* out_error);

/*
 * Frame-boundary publish operation. The caller must already have stopped all
 * old-module calls and cleared immediate-context bindings. The old module is
 * destroyed and unloaded before the prepared candidate is published. The
 * prepared slot receives the old slot's storage and, when applicable, its
 * Inactive_Copy path for explicit cleanup.
 */
Module_Loader_Result module_loader_commit(
    Module_Loader_Slot* active,
    Module_Loader_Slot* prepared,
    Module_Loader_Error* out_error);

/*
 * Destroys and unloads a prepared or active module without deleting its copied
 * DLL. The resulting state is Inactive_Copy. This is also the shutdown path.
 */
Module_Loader_Result module_loader_release(
    Module_Loader_Slot* slot,
    Module_Loader_Clear_Context_Function clear_context_bindings,
    void* clear_context_user,
    Module_Loader_Error* out_error);

/*
 * Explicitly deletes one already-unloaded hot copy. Loaded slots are rejected.
 * File-not-found is treated as an already-completed deletion.
 */
Module_Loader_Result module_loader_delete_inactive_copy(
    Module_Loader_Slot* slot,
    Module_Loader_Error* out_error);

/*
 * Forgets an inactive path without deleting it, allowing the caller to retain
 * a diagnostic copy while reusing the slot's buffers.
 */
Module_Loader_Result module_loader_forget_inactive_copy(
    Module_Loader_Slot* slot,
    Module_Loader_Error* out_error);

uint32_t module_loader_slot_is_callable(const Module_Loader_Slot* slot);
const char* module_loader_result_string(Module_Loader_Result result);

#if defined(__cplusplus)
}
#endif

#endif /* KALAM_EDITOR_HOST_MODULE_LOADER_H */
