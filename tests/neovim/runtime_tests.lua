local source = debug.getinfo(1, "S").source:sub(2)
local repo = vim.fs.dirname(vim.fs.dirname(vim.fs.dirname(source)))
local runtime = repo .. "/editor/neovim/runtime"
vim.opt.runtimepath:prepend(runtime)

local nveditor = require("nveditor")
nveditor.setup()
vim.g.nveditor_force_range_fallback = 1

local failures = 0
local function check(expression, message)
  if not expression then
    io.stderr:write("check failed: " .. message .. "\n")
    failures = failures + 1
  end
end

local first_open_event = nil
local first_open_autocmd = vim.api.nvim_create_autocmd("User", {
  pattern = "NvEditorEvent",
  callback = function(args)
    if args.data and args.data.method == "nveditor.document_valid" then
      first_open_event = args.data.payload
    end
  end,
})
local first_open_path = repo .. "/examples/cube.scene.json"
local first_open_absolute = vim.fn.fnamemodify(first_open_path, ":p")
vim.cmd.edit(vim.fn.fnameescape(first_open_path))
check(
  vim.wait(1000, function()
    return first_open_event ~= nil
  end, 10),
  "first-open JSON publishes a document event before filetype detection"
)
if first_open_event ~= nil then
  check(first_open_event.ok, "first-open document event is valid")
  check(
    vim.fs.normalize(first_open_event.path)
      == vim.fs.normalize(first_open_absolute),
    "first-open document event retains the canonical path"
  )
end
vim.api.nvim_del_autocmd(first_open_autocmd)

local valid = [[
{
  "$schema": "nveditor.scene/v1",
  "cube": {
    "color": [0.1, 0.2, 0.3, 1.0]
  }
}
]]
local result = nveditor.validate_text(valid)
check(result.ok, "valid JSON accepted")

result = nveditor.validate_text('{"a":1,"a":2}')
check(not result.ok, "literal duplicate key rejected")
check(
  result.diagnostics[1].message == "duplicate JSON object key",
  "literal duplicate diagnostic"
)

result = nveditor.validate_text('{"a":1,"\\u0061":2}')
check(not result.ok, "escaped duplicate key rejected semantically")
check(result.diagnostics[1].path == "$.a", "escaped duplicate path")

result = nveditor.validate_text('{"nested":{"x":1,"x":2}}')
check(not result.ok, "nested duplicate key rejected")
check(result.diagnostics[1].path == "$.nested.x", "nested duplicate path")

result = nveditor.validate_text('{"array":[{"id":1},{"id":2}]}')
check(result.ok, "same key in separate objects accepted")

result = nveditor.validate_text('{"a":01}')
check(not result.ok, "invalid JSON still rejected by vim.json.decode")

local buffer = vim.api.nvim_create_buf(false, true)
local utf8_prefix = '{"'
  .. string.char(0xC3, 0xA9)
  .. '":"'
  .. string.char(0xE2, 0x9C, 0x93)
  .. '","tint":'
local original_text = utf8_prefix .. '[1.000,0.500,0.250,1.000]}'
local patched_text = utf8_prefix .. '[0.100,0.200,0.300,1.000]}'
vim.api.nvim_buf_set_lines(buffer, 0, -1, true, {
  original_text,
})
vim.api.nvim_set_current_buf(buffer)
vim.bo[buffer].filetype = "json"

-- Establish the scratch-buffer seed as an existing undo state, like a file
-- loaded from disk, so the RPC patch is measured as its own undo block.
vim.cmd("silent undo")
vim.cmd("silent redo")

local snapshot = nveditor.get_buffer_text(buffer)
check(
  snapshot.text == original_text,
  "buffer text snapshot"
)

local path_context, path_error = nveditor.context_for_path(buffer, "$.tint")
check(path_context ~= nil and path_error == nil, "range-aware path lookup")
check(path_context.range_fallback == true, "forced range fallback is reported")
check(path_context.json_path == "$.tint", "path lookup identity")
check(path_context.start_row == 0, "path lookup start row")
check(path_context.start_column == 19, "path lookup UTF-8 byte start column")
check(path_context.end_column == 44, "path lookup UTF-8 byte end column")
check(
  path_context.source_text == "[1.000,0.500,0.250,1.000]",
  "path lookup visible source text"
)

local cursor_context, cursor_error = nveditor.cursor_context(buffer, 0, 14)
check(cursor_context ~= nil and cursor_error == nil, "range-aware cursor lookup")
check(cursor_context.json_path == "$.tint", "cursor resolves containing pair")

local root_context = nveditor.context_for_path(buffer, "$")
check(root_context ~= nil and root_context.node_type == "document", "root path lookup")

local missing, missing_error = nveditor.context_for_path(buffer, "$.exposure")
check(missing == nil, "missing JSON path rejected")
check(missing_error:match("not present") ~= nil, "missing path diagnostic")

local tick = snapshot.changedtick
local patch, patch_error = nveditor.patch_text({
  bufnr = buffer,
  expected_changedtick = tick,
  start_row = 0,
  start_column = 19,
  end_row = 0,
  end_column = 44,
  replacement = "[0.100,0.200,0.300,1.000]",
})
check(patch ~= nil and patch_error == nil, "surgical text patch accepted")
check(
  nveditor.get_buffer_text(buffer).text == patched_text,
  "surgical text patch changed only selected node"
)

local stale, stale_error = nveditor.patch_text({
  bufnr = buffer,
  expected_changedtick = tick,
  start_row = 0,
  start_column = 19,
  end_row = 0,
  end_column = 44,
  replacement = "[1,1,1,1]",
})
check(stale == nil, "stale changedtick patch rejected")
check(
  stale_error:match("changedtick") ~= nil,
  "stale changedtick diagnostic"
)

vim.cmd("undo")
check(
  nveditor.get_buffer_text(buffer).text == original_text,
  "one Neovim undo restores one accepted surgical patch"
)
vim.cmd("redo")
check(
  nveditor.get_buffer_text(buffer).text == patched_text,
  "Neovim redo reapplies the one surgical patch"
)

check(vim.fn.exists(":NvEditContext") == 2, "NvEditContext command exists")

check(vim.fn.exists(":Kalam") == 2, "Kalam dispatcher command exists")
check(vim.fn.exists(":NvEditContext") == 2, "NvEditContext command exists")
check(vim.fn.exists(":NvEditForm") == 2, "NvEditForm command exists")
check(vim.fn.exists(":NvBuildReload") == 2, "NvBuildReload command exists")

local actions_by_id = {}
for _, action in ipairs(nveditor.actions()) do
  actions_by_id[action.id] = action
end
for _, action_id in ipairs({
  "edit.context",
  "edit.form",
  "build.reload",
  "focus.editor",
  "focus.viewport",
  "toggle.viewport",
  "play",
  "stop",
}) do
  check(actions_by_id[action_id] ~= nil, "Kalam action is discoverable: " .. action_id)
  check(
    actions_by_id[action_id] ~= nil and actions_by_id[action_id].notification:match("^nveditor%.") ~= nil,
    "Kalam action has a stable RPC notification: " .. action_id
  )
end

local root_completion = nveditor.complete("", "Kalam ")
check(vim.tbl_contains(root_completion, "edit"), "Kalam root completion includes edit")
check(vim.tbl_contains(root_completion, "focus"), "Kalam root completion includes focus")
check(vim.tbl_contains(root_completion, "help"), "Kalam root completion includes help")

local edit_completion = nveditor.complete("", "Kalam edit ")
check(vim.tbl_contains(edit_completion, "context"), "Kalam edit completion includes context")
check(vim.tbl_contains(edit_completion, "form"), "Kalam edit completion includes form")
local focus_completion = nveditor.complete("v", "Kalam focus v")
check(#focus_completion == 1 and focus_completion[1] == "viewport", "Kalam focus completion")
local command_line_completion = vim.fn.getcompletion("Kalam edit ", "cmdline")
check(
  vim.tbl_contains(command_line_completion, "context")
    and vim.tbl_contains(command_line_completion, "form"),
  "Kalam user-command completion is installed"
)

local notifications = {}
local original_notify = vim.notify
vim.notify = function(message, level)
  table.insert(notifications, { message = message, level = level })
end
vim.cmd("Kalam")
check(
  notifications[#notifications] ~= nil
    and notifications[#notifications].message:match(":Kalam edit context") ~= nil
    and notifications[#notifications].message:match("<Plug>%(KalamEditContext%)") ~= nil,
  "Kalam with no arguments lists actions and documented Plug mappings"
)
vim.cmd("Kalam does-not-exist")
check(
  notifications[#notifications] ~= nil
    and notifications[#notifications].message:match("unknown Kalam operation") ~= nil,
  "Kalam invalid operation is concise"
)
vim.notify = original_notify

local function plug_rhs(mapping)
  local result = vim.fn.maparg(mapping, "n", false, true)
  return type(result) == "table" and result.rhs or nil
end

check(
  plug_rhs("<Plug>(KalamEditContext)") == "<Cmd>Kalam edit context<CR>",
  "Kalam contextual edit Plug mapping"
)
check(
  plug_rhs("<Plug>(KalamBuildReload)") == "<Cmd>Kalam build reload<CR>",
  "Kalam build/reload Plug mapping"
)
check(
  plug_rhs("<Plug>(KalamFocusViewport)") == "<Cmd>Kalam focus viewport<CR>",
  "Kalam viewport-focus Plug mapping"
)
check(vim.fn.maparg("<F4>", "n") == "", "F4 is not claimed by Kalam")
check(vim.fn.maparg("<S-F4>", "n") == "", "Shift-F4 is not claimed by Kalam")
check(vim.fn.maparg("<F6>", "n") == "", "F6 is not claimed by Kalam")

local dispatched_actions = {}
local action_autocmd = vim.api.nvim_create_autocmd("User", {
  pattern = "NvEditorEvent",
  callback = function(args)
    local data = args.data
    if data and data.payload and data.payload.action then
      table.insert(dispatched_actions, data)
    end
  end,
})

vim.api.nvim_win_set_cursor(0, { 1, 20 })
for _, command in ipairs({
  "Kalam edit context",
  "Kalam edit form",
  "Kalam build reload",
  "Kalam focus editor",
  "Kalam focus viewport",
  "Kalam toggle viewport",
  "Kalam play",
  "Kalam stop",
}) do
  vim.cmd(command)
end

local expected_actions = {
  { method = "nveditor.context_edit", action = "edit.context" },
  { method = "nveditor.form_edit", action = "edit.form" },
  { method = "nveditor.build_reload", action = "build.reload" },
  { method = "nveditor.focus_editor", action = "focus.editor" },
  { method = "nveditor.focus_viewport", action = "focus.viewport" },
  { method = "nveditor.toggle_viewport", action = "toggle.viewport" },
  { method = "nveditor.play", action = "play" },
  { method = "nveditor.stop", action = "stop" },
}
for index, expected in ipairs(expected_actions) do
  local dispatched = dispatched_actions[index]
  check(
    dispatched ~= nil
      and dispatched.method == expected.method
      and dispatched.payload.action == expected.action,
    "Kalam command dispatches stable action " .. expected.action
  )
end

vim.cmd("NvBuildReload")
local legacy_dispatch = dispatched_actions[#dispatched_actions]
check(
  legacy_dispatch ~= nil
    and legacy_dispatch.method == "nveditor.build_reload"
    and legacy_dispatch.payload.action == "build.reload",
  "NvBuildReload remains a shared-dispatch compatibility alias"
)
vim.api.nvim_del_autocmd(action_autocmd)
check(vim.fn.exists(":NvEditForm") == 2, "NvEditForm command exists")
check(vim.fn.exists(":NvBuildReload") == 2, "NvBuildReload command exists")

local context_events = {}
local context_event_autocmd = vim.api.nvim_create_autocmd("User", {
  pattern = "NvEditorEvent",
  callback = function(args)
    if args.data and args.data.method
        and args.data.method:match("^nveditor%.context_session_") then
      table.insert(context_events, args.data)
    end
  end,
})

local origin_tab = vim.api.nvim_get_current_tabpage()
local origin_buffer = vim.api.nvim_get_current_buf()
local prior_laststatus = vim.o.laststatus
vim.o.laststatus = 3
local session, session_error = nveditor.open_context_session({
  source_buffer = origin_buffer,
  json_path = "$.cube",
  fields = {
    { name = "tint", value_json = "[0.100,0.200,0.300,1.000]" },
    { name = "enabled", value_json = "true" },
  },
})
check(session ~= nil and session_error == nil, "context session opens")
check(
  vim.api.nvim_get_current_tabpage() ~= origin_tab,
  "context session owns a temporary Neovim tab"
)
check(vim.api.nvim_get_current_buf() == session.bufnr, "context buffer is current")
check(vim.bo[session.bufnr].buftype == "acwrite", "context buffer is acwrite")
check(vim.bo[session.bufnr].filetype == "json", "context buffer has JSON motions")
check(not vim.bo[session.bufnr].modified, "context buffer starts clean")
check(
  vim.o.laststatus == 2,
  "context tab exposes its own real window-local statusline"
)
check(
  vim.wo[session.window].statusline:match(":w apply") ~= nil
    and vim.wo[session.window].statusline:match(":q cancel") ~= nil
    and vim.wo[session.window].statusline:match(":wq apply%+close") ~= nil,
  "context window has a real Neovim command statusline"
)
check(
  table.concat(vim.api.nvim_buf_get_lines(session.bufnr, 0, -1, true), "\n")
    == '{\n  "tint": [0.100,0.200,0.300,1.000],\n  "enabled": true\n}',
  "context buffer stages named JSON fields"
)

vim.cmd("normal! j")
local moved = nveditor.context_session(session.session_id)
check(moved.cursor_row == 2, "native j moves to the next projected field")
vim.cmd("normal! k")
check(
  nveditor.context_session(session.session_id).cursor_row == 1,
  "native k moves to the previous projected field"
)

local updated, update_error = nveditor.update_context_session_field(
  session.session_id,
  "tint",
  "[0.900,0.800,0.700,1.000]"
)
check(updated ~= nil and update_error == nil, "mouse-style field write-through succeeds")
check(vim.bo[session.bufnr].modified, "mouse write-through marks Neovim modified")
check(
  vim.api.nvim_buf_get_lines(session.bufnr, 1, 2, true)[1]
    == '  "tint": [0.900,0.800,0.700,1.000],',
  "mouse write-through changes the staging line"
)
vim.cmd("undo")
check(
  vim.api.nvim_buf_get_lines(session.bufnr, 1, 2, true)[1]
    == '  "tint": [0.100,0.200,0.300,1.000],',
  "real Neovim undo restores a mouse-originated edit"
)
vim.cmd("redo")

local quit_ok = pcall(vim.cmd, "quit")
check(not quit_ok, "plain :q refuses a modified context buffer")
check(
  nveditor.context_session(session.session_id) ~= nil,
  "failed :q leaves the context session open"
)
vim.cmd("quit!")
check(vim.api.nvim_get_current_tabpage() == origin_tab, ":q! returns to the source tab")
check(
  vim.o.laststatus == 3,
  "closing the context restores the user's global-statusline setting"
)
vim.o.laststatus = prior_laststatus
local cancel_event = context_events[#context_events]
check(
  cancel_event ~= nil
    and cancel_event.method == "nveditor.context_session_closed"
    and not cancel_event.payload.write_pending,
  ":q! reports a discard rather than a write"
)

session = assert(nveditor.open_context_session({
  source_buffer = origin_buffer,
  json_path = "$.tint",
  fields = {
    { name = "tint", value_json = "[0.100,0.200,0.300,1.000]" },
  },
}))
assert(nveditor.update_context_session_field(
  session.session_id,
  "tint",
  "[0.400,0.500,0.600,1.000]"
))
vim.cmd("write")
local write_event = context_events[#context_events]
check(
  write_event ~= nil
    and write_event.method == "nveditor.context_session_write"
    and write_event.payload.session_id == session.session_id,
  ":w dispatches the staged context through RPC"
)
check(
  not vim.bo[session.bufnr].modifiable and not vim.bo[session.bufnr].modified,
  "context buffer is locked while the asynchronous write is pending"
)
assert(nveditor.finish_context_session_write(session.session_id, true, ""))
check(
  vim.bo[session.bufnr].modifiable and not vim.bo[session.bufnr].modified,
  "successful :w keeps the context session open and clean"
)

assert(nveditor.update_context_session_field(
  session.session_id,
  "tint",
  "[0.700,0.600,0.500,1.000]"
))
vim.cmd("wq")
check(vim.api.nvim_get_current_tabpage() == origin_tab, ":wq returns to the source tab")
local write_close_event = context_events[#context_events]
check(
  write_close_event ~= nil
    and write_close_event.method == "nveditor.context_session_closed"
    and write_close_event.payload.write_pending,
  ":wq closes only after dispatching a write"
)
vim.api.nvim_del_autocmd(context_event_autocmd)

vim.api.nvim_buf_delete(buffer, { force = true })

if failures ~= 0 then
  io.stderr:write(string.format("%d Neovim runtime checks failed\n", failures))
  vim.cmd.cquit(failures)
end

print("all Neovim runtime tests passed")
vim.cmd.quitall()
