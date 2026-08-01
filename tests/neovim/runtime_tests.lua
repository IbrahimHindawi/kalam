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
    and notifications[#notifications].message:match(":kalam edit context") ~= nil
    and notifications[#notifications].message:match("<Plug>%(KalamEditContext%)") ~= nil,
  "kalam with no arguments lists actions and documented Plug mappings"
)
vim.cmd("Kalam does-not-exist")
check(
  notifications[#notifications] ~= nil
    and notifications[#notifications].message:match("unknown kalam operation") ~= nil,
  "kalam invalid operation is concise"
)

check(
  vim.fn.maparg("kalam", "c", true, true).lhs == "kalam",
  "lowercase :kalam cmdline abbreviation is installed"
)

-- Selection to source: open a document and land on a key
-- (docs/entity_documents.md section 5).
local open_result, open_error =
  nveditor.open_at_json_path(first_open_path, "$.cube.color")
check(
  open_result ~= nil and open_error == nil,
  "open_at_json_path opens the document"
)
check(
  vim.fn.fnamemodify(vim.api.nvim_buf_get_name(0), ":p")
    == vim.fn.fnamemodify(first_open_path, ":p"),
  "open_at_json_path lands in the requested document"
)
if open_result ~= nil then
  local cursor = vim.api.nvim_win_get_cursor(0)
  check(
    cursor[1] == open_result.row + 1 and cursor[2] == open_result.column,
    "the cursor sits on the reported key position"
  )
  local key_line = vim.api.nvim_buf_get_lines(
    0,
    open_result.row,
    open_result.row + 1,
    false
  )[1]
  check(
    key_line ~= nil and key_line:match("color") ~= nil,
    "the cursor line holds the requested key (got '"
      .. tostring(key_line) .. "')"
  )
end
check(
  vim.api.nvim_get_mode().mode:sub(1, 1) == "n",
  "opening at a key never changes mode"
)
check(
  select(1, nveditor.open_at_json_path("", "$.cube")) == nil,
  "an empty document path is refused"
)

-- The kalam://viewport buffer contract
-- (docs/buffer_hosted_viewport.md section 3).
local viewport_events = {}
local viewport_autocmd = vim.api.nvim_create_autocmd("User", {
  pattern = "NvEditorEvent",
  callback = function(args)
    if args.data and args.data.method == "nveditor.viewport_windows" then
      table.insert(viewport_events, args.data.payload)
    end
  end,
})

local viewport_bufnr = nveditor.viewport_bufnr()
check(
  vim.api.nvim_buf_is_valid(viewport_bufnr),
  "the viewport buffer is created on demand"
)
check(
  vim.bo[viewport_bufnr].buftype == "nofile"
    and not vim.bo[viewport_bufnr].modifiable
    and vim.bo[viewport_bufnr].filetype == "kalam.viewport",
  "the viewport buffer is an unmodifiable kalam.viewport scratch buffer"
)
check(
  vim.api.nvim_buf_get_name(viewport_bufnr):match("kalam://viewport") ~= nil,
  "the viewport buffer is named kalam://viewport"
)
check(
  nveditor.viewport_bufnr() == viewport_bufnr,
  "viewport_bufnr is idempotent"
)

local viewport_mappings = { ["<CR>"] = false, ["i"] = false, ["q"] = false }
for _, mapping in ipairs(vim.api.nvim_buf_get_keymap(viewport_bufnr, "n")) do
  for wanted in pairs(viewport_mappings) do
    if mapping.lhs == wanted
        or mapping.lhs == wanted:gsub("<CR>", "\r") then
      viewport_mappings[wanted] = true
    end
  end
end
check(
  viewport_mappings["<CR>"] and viewport_mappings["i"]
    and viewport_mappings["q"],
  "the viewport buffer claims only its buffer-local mappings"
)

local function viewport_window_count()
  local count = 0
  for _, win in ipairs(vim.api.nvim_tabpage_list_wins(0)) do
    if vim.api.nvim_win_get_buf(win) == viewport_bufnr then
      count = count + 1
    end
  end
  return count
end

nveditor.viewport_toggle()
check(
  viewport_window_count() == 1,
  "toggling opens one viewport window"
)
nveditor.viewport_open()
check(
  viewport_window_count() == 1,
  "opening again reuses the existing window"
)
vim.wait(200, function()
  return #viewport_events > 0
end)
check(
  #viewport_events > 0
    and #viewport_events[#viewport_events].windows == 1
    and viewport_events[#viewport_events].active ~= 0,
  "opening the viewport reports its window set"
)

local before_close = #viewport_events
nveditor.viewport_toggle()
check(
  viewport_window_count() == 0,
  "toggling again closes the viewport window"
)
vim.wait(200, function()
  return #viewport_events > before_close
end)
check(
  #viewport_events > before_close
    and #viewport_events[#viewport_events].windows == 0,
  "closing the viewport reports an empty window set"
)
vim.api.nvim_del_autocmd(viewport_autocmd)
local help_count_before = #notifications
vim.fn.feedkeys(":kalam\r", "tx")
check(
  #notifications > help_count_before
    and notifications[#notifications].message:match("kalam actions") ~= nil,
  "typing :kalam reaches the dispatcher through the abbreviation"
)
vim.fn.feedkeys(":let g:kalam_abbrev_probe = 'kalam kept'\r", "tx")
check(
  vim.g.kalam_abbrev_probe == "kalam kept",
  "the abbreviation leaves mid-line kalam text alone"
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
  "Kalam play",
  "Kalam stop",
}) do
  vim.cmd(command)
end

-- toggle.viewport is deliberately absent: it acts on the Neovim layout
-- locally (docs/buffer_hosted_viewport.md) and sends no action
-- notification; its behavior is covered by the viewport contract tests.
local expected_actions = {
  { method = "nveditor.context_edit", action = "edit.context" },
  { method = "nveditor.form_edit", action = "edit.form" },
  { method = "nveditor.build_reload", action = "build.reload" },
  { method = "nveditor.focus_editor", action = "focus.editor" },
  { method = "nveditor.focus_viewport", action = "focus.viewport" },
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

local context_dispatch = dispatched_actions[1]
check(
  context_dispatch ~= nil
    and type(context_dispatch.payload.screen_row) == "number"
    and context_dispatch.payload.screen_row >= 0
    and type(context_dispatch.payload.screen_column) == "number"
    and context_dispatch.payload.screen_column >= 0,
  "context dispatch captures the source cursor screen position"
)

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

vim.api.nvim_buf_delete(buffer, { force = true })

if failures ~= 0 then
  io.stderr:write(string.format("%d Neovim runtime checks failed\n", failures))
  vim.cmd.cquit(failures)
end

print("all Neovim runtime tests passed")
vim.cmd.quitall()
