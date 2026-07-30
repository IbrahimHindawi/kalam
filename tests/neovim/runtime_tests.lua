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
check(vim.fn.exists(":NvEditForm") == 2, "NvEditForm command exists")
check(vim.fn.exists(":NvBuildReload") == 2, "NvBuildReload command exists")

vim.api.nvim_buf_delete(buffer, { force = true })

if failures ~= 0 then
  io.stderr:write(string.format("%d Neovim runtime checks failed\n", failures))
  vim.cmd.cquit(failures)
end

print("all Neovim runtime tests passed")
vim.cmd.quitall()
