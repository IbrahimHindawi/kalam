local channel, path, palette, gui_workspace = ...
palette = palette or {background = "#242529", alternate = "#292b2f", panel = "#303236",
  text = "#e3dfd8", muted = "#a39d94", accent = "#dfa26c", selected = "#554233"}
vim.cmd("filetype on")
if not vim.g.syntax_on then vim.cmd("syntax enable") end
vim.cmd.edit(vim.fn.fnameescape(path))
local source = vim.api.nvim_get_current_buf()
local viewport
local viewport_views = {}
local entity_dir = vim.fs.normalize((path:gsub("%.json$", "")))

local function valid_entity_id(id)
  return type(id) == "string" and #id <= 64 and id:match("^[A-Za-z0-9_-]+$") ~= nil
end

local function entity_path(id)
  return vim.fs.joinpath(entity_dir, id .. ".json")
end

local function switch_buffer(win, buffer)
  vim.api.nvim_win_call(win, function() vim.cmd("hide buffer " .. buffer) end)
end

local function publish()
  local target = 0
  for _, win in ipairs(vim.api.nvim_tabpage_list_wins(0)) do
    if vim.api.nvim_win_get_buf(win) == viewport then
      target = win
      if win == vim.api.nvim_get_current_win() then break end
    end
  end
  vim.rpcnotify(channel, "kalam.viewport", target, vim.api.nvim_get_current_win())
end

local viewport_options = { number = false, relativenumber = false, signcolumn = "no", foldcolumn = "0" }

local function show_viewport(win)
  if not viewport or not vim.api.nvim_buf_is_valid(viewport) then
    viewport = vim.api.nvim_create_buf(false, true)
    vim.api.nvim_buf_set_name(viewport, "kalam://viewport")
    vim.bo[viewport].bufhidden = "hide"
    vim.bo[viewport].modifiable = false
    vim.bo[viewport].filetype = "kalam.viewport"
  end
  local view = { buffer = vim.api.nvim_win_get_buf(win), options = {} }
  for name in pairs(viewport_options) do view.options[name] = vim.wo[win][name] end
  switch_buffer(win, viewport)
  for name, value in pairs(viewport_options) do vim.wo[win][name] = value end
  viewport_views[win] = view
end

local function restore_viewport(win)
  local view = viewport_views[win]
  local buffer = view and vim.api.nvim_buf_is_valid(view.buffer) and view.buffer or source
  if not vim.api.nvim_buf_is_valid(buffer) then buffer = vim.api.nvim_create_buf(true, false) end
  switch_buffer(win, buffer)
  if view then
    for name, value in pairs(view.options) do vim.wo[win][name] = value end
  end
  viewport_views[win] = nil
end

local function open()
  for _, win in ipairs(vim.api.nvim_tabpage_list_wins(0)) do
    if vim.api.nvim_win_get_buf(win) == viewport then
      vim.api.nvim_set_current_win(win)
      publish()
      return
    end
  end
  vim.cmd("botright vsplit")
  show_viewport(vim.api.nvim_get_current_win())
  publish()
end

local group = vim.api.nvim_create_augroup("KalamViewport", {clear = true})
vim.api.nvim_create_autocmd({"BufWinEnter", "BufWinLeave", "WinEnter", "WinClosed", "TabEnter"}, {
  group = group,
  callback = function(event)
    if event.event == "WinEnter" then publish()
    else vim.schedule(publish) end
  end,
})
vim.api.nvim_create_autocmd("BufWritePost", {
  group = group, pattern = "*.json",
  callback = function(event)
    local name = vim.fs.normalize(vim.api.nvim_buf_get_name(event.buf))
    if event.buf == source or vim.fs.dirname(name) == entity_dir then
      vim.rpcnotify(channel, "kalam.reload")
    end
  end,
})

local function live_path(name)
  name = vim.fs.normalize(name)
  return name == vim.fs.normalize(path) or
    name == vim.fs.joinpath(vim.fs.dirname(vim.fs.dirname(entity_dir)), "materials", "reference.json") or
    (vim.fs.dirname(name) == entity_dir and name:lower():match("%.json$") ~= nil)
end

local live_pending = false
local function publish_live_documents()
  if live_pending then return end
  live_pending = true
  vim.schedule(function()
    live_pending = false
    local docs = vim.empty_dict()
    for _, info in ipairs(vim.fn.getbufinfo({bufmodified = 1})) do
      if vim.api.nvim_buf_is_loaded(info.bufnr) and live_path(info.name) then
        docs[info.name] = table.concat(vim.api.nvim_buf_get_lines(info.bufnr, 0, -1, false), "\n")
      end
    end
    vim.rpcnotify(channel, "kalam.live_documents", vim.json.encode(docs))
  end)
end

vim.api.nvim_create_autocmd({"TextChanged", "TextChangedI", "TextChangedP", "BufModifiedSet",
  "BufWritePost", "BufUnload"}, {
  group = group,
  callback = function(event)
    if live_path(vim.api.nvim_buf_get_name(event.buf)) then publish_live_documents() end
  end,
})
vim.api.nvim_create_autocmd("User", {
  group = group, pattern = "KalamFormsRefresh", callback = publish_live_documents,
})
vim.api.nvim_create_autocmd({"CmdlineEnter", "CmdlineLeave"}, {
  group = group,
  callback = function(event)
    vim.rpcnotify(channel, "kalam.cmdline", event.event == "CmdlineEnter")
  end,
})
local forms = false
local pending = false

local function is_json(buffer)
  return vim.bo[buffer].buftype == "" and
    (vim.bo[buffer].filetype == "json" or vim.api.nvim_buf_get_name(buffer):lower():match("%.json$") ~= nil)
end

local function format_json(buffer, join_undo)
  if not is_json(buffer) or not vim.bo[buffer].modifiable then return end
  local prettier = vim.fn.exepath("prettier")
  local command = {prettier, "--parser", "json", "--print-width", "80"}
  if vim.fn.has("win32") == 1 then
    local shim = vim.fn.exepath("prettier.cmd")
    local script = shim ~= "" and vim.fs.joinpath(vim.fs.dirname(shim),
      "node_modules", "prettier", "bin", "prettier.cjs")
    if script and vim.fn.filereadable(script) == 1 then
      command = {"node", script, "--parser", "json", "--print-width", "80"}
    elseif prettier:lower():match("%.cmd$") then
      prettier = ""
    end
  end
  if prettier == "" and command[1] ~= "node" then
    if not join_undo then vim.notify("Install Prettier with npm install -g prettier.", vim.log.levels.WARN) end
    return
  end
  local text = table.concat(vim.api.nvim_buf_get_lines(buffer, 0, -1, false), "\n") .. "\n"
  -- The editor accepts strict JSON. Keep invalid documents intact for repair.
  if not pcall(vim.json.decode, text) then
    if not join_undo then vim.notify("Fix invalid JSON before formatting.", vim.log.levels.WARN) end
    return
  end
  local ok, result = pcall(function()
    return vim.system(command, {stdin = text, text = true}):wait(3000)
  end)
  if not ok or result.code ~= 0 then
    vim.notify("Prettier could not format this buffer.", vim.log.levels.WARN)
    return
  end
  if result.stdout ~= text then
    if join_undo and vim.bo[buffer].modified then
      vim.api.nvim_buf_call(buffer, function() pcall(vim.cmd.undojoin) end)
    end
    vim.api.nvim_buf_set_lines(buffer, 0, -1, false,
      vim.split((result.stdout:gsub("\n$", "")), "\n", {plain = true}))
  end
end

vim.api.nvim_create_user_command("KalamFormatJson", function()
  format_json(vim.api.nvim_get_current_buf(), false)
end, {desc = "Format the current JSON buffer with Prettier"})
vim.api.nvim_create_autocmd("BufWritePre", {
  group = group, pattern = "*.json",
  callback = function(event) format_json(event.buf, true) end,
})

local function document(win, buffer)
  buffer = buffer or vim.api.nvim_win_get_buf(win)
  if not is_json(buffer) then return end
  local lines = vim.api.nvim_buf_get_lines(buffer, 0, -1, false)
  local text = table.concat(lines, "\n")
  if #text > 256 * 1024 then return end
  return {win, buffer, vim.api.nvim_buf_get_changedtick(buffer), vim.api.nvim_buf_get_name(buffer), text}, lines
end

local tui = {}
local panels = {}
local tui_marks = vim.api.nvim_create_namespace("kalam.inspector")
local function inspector_highlights()
  local highlights = {
    KalamInspector = {fg = palette.text, bg = palette.background},
    KalamInspectorAlternate = {fg = palette.text, bg = palette.alternate},
    KalamInspectorHeader = {fg = palette.text, bg = palette.panel, bold = true},
    KalamInspectorSection = {fg = palette.text, bg = palette.panel, bold = true},
    KalamInspectorSelected = {fg = palette.text, bg = palette.selected},
    KalamInspectorMuted = {fg = palette.muted},
    KalamInspectorValue = {fg = palette.accent},
  }
  for name, colors in pairs(highlights) do vim.api.nvim_set_hl(0, name, colors) end
end
inspector_highlights()
vim.api.nvim_create_autocmd("ColorScheme", {group = group, callback = inspector_highlights})

local function document_controls(data, live_scene)
  local controls = {}
  local function add(value, binding, label, depth, parent)
    local container = type(value) == "table" and next(value) ~= nil and depth < 64
    if binding ~= "" or type(value) ~= "table" or next(value) == nil then
      local control = {label = label, bind = binding, depth = math.max(0, depth - 1), parent = parent,
        group = container}
      if live_scene and binding == "/exposure" and type(value) == "number" then
        control.min, control.max, control.step = 0.01, 32, 0.1
      end
      controls[#controls + 1] = control
      parent = #controls
    end
    if not container then return end
    if vim.islist(value) then
      for index, child in ipairs(value) do
        local key = tostring(index - 1)
        add(child, binding .. "/" .. key, "[" .. key .. "]", depth + 1, parent)
      end
    else
      local keys = vim.tbl_keys(value)
      table.sort(keys)
      for _, key in ipairs(keys) do
        local part = key:gsub("~", "~0"):gsub("/", "~1")
        local child_label = key:match("^[%w_%-]+$") and key or "[" .. vim.json.encode(key) .. "]"
        add(value[key], binding .. "/" .. part, child_label, depth + 1, parent)
      end
    end
  end
  add(data, "", "$", 0)
  return controls
end

local function bound_value(root, binding)
  if binding == "" then return root end
  local value = root
  for part in (binding:sub(2) .. "/"):gmatch("(.-)/") do
    if type(value) ~= "table" then return end
    part = part:gsub("~1", "/"):gsub("~0", "~")
    if vim.islist(value) then
      local index = tonumber(part)
      if not index or tostring(index) ~= part or index < 0 or index % 1 ~= 0 then return end
      part = index + 1
    end
    value = value[part]
  end
  return value
end

local function exposure_from_json(text)
  local ok, value = pcall(vim.json.decode, text)
  if ok and type(value) == "table" and type(value.exposure) == "number" and
      value.exposure >= 0.01 and value.exposure <= 32 then
    return value.exposure
  end
end

local function column(text, width)
  text = text:gsub("[%c]", " ")
  if vim.fn.strdisplaywidth(text) > width then
    text = vim.fn.strcharpart(text, 0, width - 1)
    while vim.fn.strdisplaywidth(text) > width - 1 do
      text = vim.fn.strcharpart(text, 0, vim.fn.strchars(text) - 1)
    end
    text = text .. "…"
  end
  return text .. string.rep(" ", math.max(0, width - vim.fn.strdisplaywidth(text)))
end

local function display_value(value)
  if value == nil then return "Unresolved binding" end
  if type(value) == "boolean" then return value and "[x] On" or "[ ] Off" end
  if type(value) == "table" and not vim.islist(value) then
    return string.format("%d fields", vim.tbl_count(value))
  end
  if type(value) == "table" then
    if #value >= 2 and #value <= 4 and vim.iter(value):all(function(v) return type(v) == "number" end) then
      local parts = {}
      for index, number in ipairs(value) do
        parts[index] = string.format("%s %.6g", ("XYZW"):sub(index, index), number)
      end
      return table.concat(parts, "  ")
    end
    return string.format("%d items", #value)
  end
  return vim.json.encode(value)
end

local function draw_tui(state)
  local width = math.max(8, vim.api.nvim_win_get_width(state.win))
  local labels = math.max(4, math.min(40, math.floor(width * 0.44)))
  local values = math.max(2, width - labels - 3)
  local lines = {state.panel.title:gsub("[%c]", " "),
    column(" Parameter", labels) .. " │ " .. column("Value", values)}
  local visible, hidden = {}, {}
  for index, control in ipairs(state.panel.controls) do
    local parent = control.parent and state.panel.controls[control.parent]
    if parent and (hidden[control.parent] or state.collapsed[parent.bind]) then
      hidden[index] = true
    else
      visible[#visible + 1] = index
    end
  end
  if hidden[state.row] then state.row = visible[1] or 1 end
  state.visible = visible
  local cursor = 2
  local styles = {}
  for _, index in ipairs(visible) do
    local control = state.panel.controls[index]
    local value = state.values[index]
    local indent = math.min((control.depth or 0) * 2, math.max(0, labels - 5))
    local disclosure = control.group and (state.collapsed[control.bind] and "▸ " or "▾ ") or "  "
    local label = (index == state.row and "›" or " ") .. string.rep(" ", indent) .. disclosure .. control.label
    local summary = display_value(value)
    if type(value) == "number" and control.min and control.max then
      local ticks = math.max(3, math.min(16, values - 9))
      local fraction = math.max(0, math.min(1, (value - control.min) / (control.max - control.min)))
      local filled = math.floor(ticks * fraction + 0.5)
      summary = string.format("%s%s  %.7g", string.rep("━", filled), string.rep("─", ticks - filled), value)
    end
    local gutter = column(label, labels) .. " │ "
    lines[#lines + 1] = gutter .. column(summary, values)
    styles[#lines] = {start = #gutter, group = index == state.row and "KalamInspectorSelected"
      or control.group and "KalamInspectorSection"
      or #lines % 2 == 0 and "KalamInspectorAlternate" or "KalamInspector",
      value = (type(value) == "table" or value == nil) and "KalamInspectorMuted" or "KalamInspectorValue"}
    if index == state.row then cursor = #lines end
  end
  lines[#lines + 1] = state.message or "h/l adjust · i edit · Enter apply · Esc cancel"
  vim.bo[state.scratch].modifiable = true
  vim.api.nvim_buf_set_lines(state.scratch, 0, -1, false, lines)
  vim.bo[state.scratch].modifiable = false
  vim.api.nvim_buf_clear_namespace(state.scratch, tui_marks, 0, -1)
  for line = 1, 2 do
    vim.api.nvim_buf_set_extmark(state.scratch, tui_marks, line - 1, 0, {line_hl_group = "KalamInspectorHeader"})
  end
  for line, mark in pairs(styles) do
    vim.api.nvim_buf_set_extmark(state.scratch, tui_marks, line - 1, 0, {line_hl_group = mark.group})
    vim.api.nvim_buf_set_extmark(state.scratch, tui_marks, line - 1, mark.start,
      {end_col = #lines[line], hl_group = mark.value})
  end
  vim.api.nvim_win_set_cursor(state.win, {cursor, 0})
end

local function refresh_tui(state)
  state.doc = vim.api.nvim_buf_is_loaded(state.buffer) and document(state.win, state.buffer) or nil
  local ok, data = false, nil
  if state.doc then ok, data = pcall(vim.json.decode, state.doc[5]) end
  if state.automatic then
    local previous = state.panel.controls[state.row]
    state.panel.controls = ok and document_controls(data, state.buffer == source) or {}
    state.row = 1
    for index, control in ipairs(state.panel.controls) do
      if control.group and state.collapsed[control.bind] == nil then
        state.collapsed[control.bind] = control.depth > 0
      end
      if control.bind == (previous and previous.bind or "/exposure") then
        state.row = index
      end
    end
    if not ok then
      state.message = "Open valid JSON up to 256 KiB or use :KalamMode text to repair it."
    end
  end
  state.values = {}
  if ok then
    for index, control in ipairs(state.panel.controls) do
      state.values[index] = bound_value(data, control.bind)
    end
  end
  state.original = vim.deepcopy(state.values)
  draw_tui(state)
end

local function publish_source()
  if not vim.api.nvim_buf_is_loaded(source) then return end
  local text = table.concat(vim.api.nvim_buf_get_lines(source, 0, -1, false), "\n")
  vim.rpcnotify(channel, "kalam.buffer_revision", source, vim.api.nvim_buf_get_changedtick(source),
    #text <= 256 * 1024 and exposure_from_json(text) or 0)
end

local function cancel_tui(state)
  if state.pending then return end
  state.message = nil
  vim.rpcnotify(channel, "kalam.cancel_preview", state.win)
  refresh_tui(state)
end

local function close_tui(win)
  local state = tui[win]
  if not state then return true end
  if state.pending then return false end
  tui[win] = nil
  vim.rpcnotify(channel, "kalam.cancel_preview", win)
  if vim.api.nvim_win_get_buf(win) == state.scratch and vim.api.nvim_buf_is_valid(state.buffer) then
    vim.api.nvim_win_set_buf(win, state.buffer)
    for option, value in pairs(state.options) do vim.wo[win][option] = value end
    vim.api.nvim_win_call(win, function() vim.fn.winrestview(state.view) end)
  end
  if vim.api.nvim_buf_is_valid(state.scratch) then
    vim.api.nvim_buf_delete(state.scratch, {force = true})
  end
  return true
end

local function open_tui(win)
  if tui[win] then return end
  local buffer = vim.api.nvim_win_get_buf(win)
  if not is_json(buffer) then
    vim.notify("Open a JSON document first.", vim.log.levels.WARN)
    return
  end
  local scratch = vim.api.nvim_create_buf(false, true)
  local panel = panels[win]
  local inspector_options = {number = false, relativenumber = false, signcolumn = "no", foldcolumn = "0",
    wrap = false, cursorline = false, winhighlight = "Normal:KalamInspector,NormalNC:KalamInspector",
    winbar = "%#KalamInspectorHeader# Inspector %=%#KalamInspectorMuted# j/k select · za fold "}
  local options = {}
  for option in pairs(inspector_options) do options[option] = vim.wo[win][option] end
  local state = {win = win, buffer = buffer, scratch = scratch, view = vim.fn.winsaveview(),
    options = options, row = 1, automatic = panel == nil, collapsed = {},
    panel = panel or {title = vim.fn.fnamemodify(vim.api.nvim_buf_get_name(buffer), ":t"), controls = {}}}
  tui[win] = state
  vim.bo[scratch].filetype = "kalam.tui"
  vim.bo[scratch].bufhidden = "wipe"
  vim.api.nvim_create_autocmd("BufWipeout", {
    group = group, buffer = scratch, once = true,
    callback = function()
      if tui[win] == state then
        tui[win] = nil
        vim.rpcnotify(channel, "kalam.cancel_preview", win)
        vim.schedule(function()
          if not tui[win] and vim.api.nvim_win_is_valid(win) then
            for option, value in pairs(options) do vim.wo[win][option] = value end
          end
        end)
      end
    end,
  })
  vim.api.nvim_win_set_buf(win, scratch)
  for option, value in pairs(inspector_options) do vim.wo[win][option] = value end
  local function send(value, apply)
    if state.pending or not state.doc or state.values[state.row] == nil then return end
    if vim.api.nvim_buf_get_changedtick(buffer) ~= state.doc[3] then
      cancel_tui(state)
      vim.notify("JSON buffer changed. The form has been refreshed.", vim.log.levels.WARN)
      return
    end
    state.message = nil
    state.values[state.row] = value
    state.pending = apply
    local request = vim.list_extend(vim.deepcopy(state.doc),
      {state.panel.controls[state.row].bind, vim.json.encode(value), apply})
    vim.rpcnotify(channel, "kalam.tui_edit", unpack(request))
    draw_tui(state)
  end
  local function nudge(direction)
    local control = state.panel.controls[state.row]
    if not control then return end
    local value = state.values[state.row]
    if control.choices and value ~= nil then
      local choices = vim.tbl_filter(function(choice) return type(choice) == type(value) end, control.choices)
      for index, choice in ipairs(choices) do
        if vim.deep_equal(choice, value) then
          send(choices[(index - 1 + direction * vim.v.count1) % #choices + 1], false)
          return
        end
      end
      if #choices > 0 then send(choices[1], false) end
    elseif type(value) == "number" then
      value = tonumber(string.format("%.12g", value + direction * (control.step or 0.1) * vim.v.count1))
      if control.min then value = math.max(control.min, value) end
      if control.max then value = math.min(control.max, value) end
      send(value, false)
    elseif type(value) == "boolean" then
      send(direction > 0, false)
    end
  end
  local map = function(key, callback) vim.keymap.set("n", key, callback, {buffer = scratch}) end
  local function move(direction)
    if state.pending or #state.visible == 0 then return end
    if not vim.deep_equal(state.values[state.row], state.original[state.row]) then cancel_tui(state) end
    local position = vim.fn.index(state.visible, state.row) + 1
    state.row = state.visible[math.max(1, math.min(#state.visible, position + direction * vim.v.count1))]
    draw_tui(state)
  end
  local function toggle_section()
    local control = state.panel.controls[state.row]
    if state.pending or not control or not control.group then return end
    state.collapsed[control.bind] = not state.collapsed[control.bind]
    draw_tui(state)
  end
  map("h", function() nudge(-1) end)
  map("l", function() nudge(1) end)
  map("j", function() move(1) end)
  map("k", function() move(-1) end)
  map("za", toggle_section)
  map("i", function()
    local value, row, revision = state.values[state.row], state.row, state.doc and state.doc[3]
    if state.pending or value == nil then return end
    vim.ui.input({prompt = state.panel.controls[row].label .. " (JSON): ", default = vim.json.encode(value)}, function(input)
      if not input or tui[win] ~= state or state.row ~= row or not state.doc or state.doc[3] ~= revision then return end
      local ok, next_value = pcall(vim.json.decode, input)
      if ok then
        send(next_value, false)
      else
        state.message = "Enter a valid JSON value."
        draw_tui(state)
      end
    end)
  end)
  map("<CR>", function()
    if not vim.deep_equal(state.values[state.row], state.original[state.row]) then
      send(state.values[state.row], true)
    else
      toggle_section()
    end
  end)
  map("<Esc>", function() cancel_tui(state) end)
  if buffer == source then publish_source() end
  refresh_tui(state)
end

vim.api.nvim_create_autocmd("WinResized", {
  group = group,
  callback = function()
    for _, win in ipairs(vim.v.event.windows or {}) do
      local state = tui[win]
      if state then draw_tui(state) end
    end
  end,
})

vim.api.nvim_create_autocmd("User", {
  group = group, pattern = "KalamTuiApplied",
  callback = function(event)
    local state = event.data and tui[event.data.window]
    if state then
      state.pending = false
      state.message = event.data.message ~= "" and event.data.message or nil
      refresh_tui(state)
    end
  end,
})
vim.api.nvim_create_autocmd("WinClosed", {
  group = group,
  callback = function(event)
    local win = tonumber(event.match)
    panels[win] = nil
    vim.rpcnotify(channel, "kalam.form_closed", win)
    local state = tui[win]
    if not state then return end
    tui[win] = nil
    vim.rpcnotify(channel, "kalam.cancel_preview", win)
    vim.schedule(function()
      if vim.api.nvim_buf_is_valid(state.scratch) then
        vim.api.nvim_buf_delete(state.scratch, {force = true})
      end
    end)
  end,
})
vim.api.nvim_create_autocmd({"TextChanged", "TextChangedI", "BufReadPost"}, {
  group = group, callback = function(event)
    local buffer = event.buf
    vim.schedule(function()
      if buffer == source then publish_source() end
      if not vim.api.nvim_buf_is_loaded(buffer) then return end
      local tick = vim.api.nvim_buf_get_changedtick(buffer)
      for _, state in pairs(tui) do
        if state.buffer == buffer and not state.pending and (not state.doc or state.doc[3] ~= tick) then
          refresh_tui(state)
        end
      end
    end)
  end,
})

local function show_forms()
  pending = false
  if not forms then return end
  local visible = {}
  for _, win in ipairs(vim.api.nvim_tabpage_list_wins(0)) do
    local doc = document(win)
    if doc then
      if panels[win] and doc[2] == source then doc[6] = vim.json.encode(panels[win]) end
      table.insert(visible, doc)
    end
  end
  vim.rpcnotify(channel, "kalam.forms", forms, visible)
end

vim.api.nvim_create_autocmd("User", {
  group = group, pattern = "KalamFormsRefresh", callback = show_forms,
})

vim.api.nvim_create_autocmd({"BufWinEnter", "WinClosed", "TabEnter", "TextChanged", "TextChangedI"}, {
  group = group,
  callback = function()
    if not forms or pending then return end
    pending = true
    vim.schedule(show_forms)
  end,
})

local function field()
  local win = vim.api.nvim_get_current_win()
  local doc, lines = document(win)
  if not doc then
    vim.notify("Open a JSON buffer up to 256 KiB first.", vim.log.levels.WARN)
    return
  end
  local cursor = vim.api.nvim_win_get_cursor(win)
  local offset = cursor[2]
  for row = 1, cursor[1] - 1 do offset = offset + #lines[row] + 1 end
  table.insert(doc, offset)
  vim.rpcnotify(channel, "kalam.field", unpack(doc))
end

vim.api.nvim_create_user_command("KalamField", field, {desc = "Edit the JSON field at the cursor"})
local function set_mode(mode, win)
  if mode == "tui" then
    open_tui(win)
    if forms then show_forms() end
    return
  end
  if not close_tui(win) then return end
  forms = mode == "gui"
  if forms then show_forms() else vim.rpcnotify(channel, "kalam.forms", false, {}) end
end


local function tui_file(path)
  if vim.fn.filereadable(path) == 0 then
    local relative = path:match("^assets[/\\](.+)$")
    local assets = vim.fs.dirname(vim.fs.dirname(vim.api.nvim_buf_get_name(source)))
    local candidate = relative and vim.fs.joinpath(assets, relative)
    if not candidate or vim.fn.filereadable(candidate) == 0 then
      vim.notify("JSON file is unreadable: " .. path, vim.log.levels.WARN)
      return
    end
    path = candidate
  end
  return vim.fn.fnamemodify(path, ":p")
end

vim.api.nvim_create_user_command("KalamMode", function(command)
  local mode, path = command.fargs[1], command.fargs[2]
  if (mode ~= "text" and mode ~= "gui" and mode ~= "tui") or
      (path and mode ~= "tui") or #command.fargs > 2 then
    vim.notify("Use :KalamMode text, gui, or tui [file.json]", vim.log.levels.WARN)
    return
  end
  local win = vim.api.nvim_get_current_win()
  if mode == "tui" then
    if path then
      path = tui_file(path)
      if not path then return end
    end
    if tui[win] and (path or not tui[win].automatic) and not close_tui(win) then return end
    if path then
      -- Load alongside the old buffer so unsaved edits remain available.
      local buffer = vim.fn.bufadd(vim.fn.fnamemodify(path, ":p"))
      vim.fn.bufload(buffer)
      vim.api.nvim_win_set_buf(win, buffer)
    end
    panels[win] = nil
  end
  set_mode(mode, win)
end, {nargs = "+", complete = function(arg, line)
  if line:match("^%s*KalamMode%s+tui%s+") then return vim.fn.getcompletion(arg, "file") end
  return vim.tbl_filter(function(mode) return vim.startswith(mode, arg) end, {"text", "tui", "gui"})
end})

local dock_edges = {left = "H", right = "L", top = "K", bottom = "J"}
vim.api.nvim_create_user_command("KalamDock", function(command)
  local edge, file = command.fargs[1], command.fargs[2]
  if not dock_edges[edge] or #command.fargs > 2 then
    vim.notify("Use :KalamDock left|right|top|bottom [file.json]", vim.log.levels.WARN)
    return
  end
  if file then
    file = tui_file(file)
    if not file then return end
    local buffer = vim.fn.bufadd(file)
    vim.fn.bufload(buffer)
    if not is_json(buffer) then
      vim.notify("The dock needs a JSON document.", vim.log.levels.WARN)
      return
    end
    if edge == "left" or edge == "right" then
      vim.cmd("vertical sbuffer " .. buffer)
    else
      vim.cmd.sbuffer(buffer)
    end
    set_mode("tui", vim.api.nvim_get_current_win())
  end
  vim.cmd.wincmd(dock_edges[edge])
end, {nargs = "+", complete = function(arg, line)
  if line:match("^%s*KalamDock%s+%w+%s+") then return vim.fn.getcompletion(arg, "file") end
  return vim.tbl_filter(function(edge) return vim.startswith(edge, arg) end,
    {"left", "right", "top", "bottom"})
end})

vim.api.nvim_create_user_command("KalamPanel", function(command)
  local mode, path = command.fargs[1], command.fargs[2]
  if mode ~= "tui" and mode ~= "gui" or #command.fargs > 2 then
    vim.notify("Use :KalamPanel tui|gui [descriptor.json]", vim.log.levels.WARN)
    return
  end
  local ok, lines
  if path then
    if vim.fn.getfsize(path) > 64 * 1024 then
      vim.notify("Panel descriptor exceeds 64 KiB.", vim.log.levels.WARN)
      return
    end
    ok, lines = pcall(vim.fn.readfile, path)
  else
    ok, lines = true, vim.api.nvim_buf_get_lines(0, 0, -1, false)
  end
  local text = ok and table.concat(lines, "\n") or ""
  local parsed, panel = pcall(vim.json.decode, text)
  local valid = #text <= 64 * 1024 and parsed and type(panel) == "table" and panel.version == 1 and
    type(panel.title) == "string" and panel.title ~= "" and vim.islist(panel.controls) and
    #panel.controls > 0 and #panel.controls <= 32
  if valid then
    local bindings = {}
    for _, control in ipairs(panel.controls) do
      if type(control) ~= "table" or type(control.label) ~= "string" or control.label == "" or
          type(control.bind) ~= "string" or control.bind:sub(1, 1) ~= "/" or bindings[control.bind] or
          (control.min ~= nil and type(control.min) ~= "number") or
          (control.max ~= nil and type(control.max) ~= "number") or
          (control.min and control.max and control.min >= control.max) or
          (control.step ~= nil and (type(control.step) ~= "number" or control.step <= 0)) or
          (control.choices ~= nil and (not vim.islist(control.choices) or
            #control.choices == 0 or #control.choices > 32)) then
        valid = false
        break
      end
      bindings[control.bind] = true
    end
  end
  if not valid then
    vim.notify("Expected a version 1 panel with a title and ordered field bindings.", vim.log.levels.WARN)
    return
  end
  vim.cmd.sbuffer(source)
  local win = vim.api.nvim_get_current_win()
  panels[win] = panel
  set_mode(mode, win)
end, {nargs = "+"})
vim.api.nvim_create_user_command("KalamFocusForm", function()
  if forms then vim.rpcnotify(channel, "kalam.form_focus", vim.api.nvim_get_current_win()) end
end, {desc = "Give focus to the current pane's generated form"})

local function map_field(event)
  local buffer = event.buf
  if is_json(buffer) and not vim.b[buffer].kalam_field_mapped then
    vim.b[buffer].kalam_field_mapped = true
    vim.api.nvim_buf_call(buffer, function()
      if vim.fn.maparg("<F2>", "n") == "" then
        vim.keymap.set("n", "<F2>", field, {buffer = buffer, desc = "Open the JSON field control"})
      end
    end)
  end
end
vim.api.nvim_create_autocmd("BufEnter", {group = group, callback = map_field})
map_field({buf = source})

local function inspect()
  if not vim.api.nvim_buf_is_valid(source) then return end
  local text = table.concat(vim.api.nvim_buf_get_lines(source, 0, -1, false), "\n")
  local ok, manifest = pcall(vim.json.decode, text)
  local records = {}
  if ok and type(manifest) == "table" and type(manifest.entities) == "table" then
    for _, id in ipairs(manifest.entities) do
      if valid_entity_id(id) then
        local file = entity_path(id)
        local buffer = vim.fn.bufnr(file)
        if buffer ~= -1 or vim.fn.filereadable(file) == 1 then
          if buffer == -1 then buffer = vim.fn.bufadd(file) end
          vim.fn.bufload(buffer)
          records[id] = {
            buffer = buffer,
            revision = vim.api.nvim_buf_get_changedtick(buffer),
            text = table.concat(vim.api.nvim_buf_get_lines(buffer, 0, -1, false), "\n"),
          }
        end
      end
    end
  end
  vim.rpcnotify(channel, "kalam.inspect", source, vim.api.nvim_buf_get_changedtick(source),
    text, vim.json.encode(next(records) and records or vim.empty_dict()))
end

vim.api.nvim_create_user_command("KalamEntities", inspect, {desc = "Edit the scene's ECS entities"})
vim.keymap.set("n", "<leader>ke", inspect, {buffer = source, desc = "Edit Kalam entities"})
local function focus_gui_viewport()
  vim.rpcnotify(channel, "kalam.focus_viewport")
end
vim.api.nvim_create_user_command("KalamViewport", gui_workspace and focus_gui_viewport or open,
  {desc = "Open or focus the Kalam viewport"})
vim.api.nvim_create_user_command("Viewport", function()
  if gui_workspace then return focus_gui_viewport() end
  local win = vim.api.nvim_get_current_win()
  if vim.api.nvim_win_get_config(win).relative ~= "" then return end
  if vim.api.nvim_win_get_buf(win) == viewport then
    restore_viewport(win)
  else
    if not close_tui(win) then return end
    for _, other in ipairs(vim.api.nvim_tabpage_list_wins(0)) do
      if other ~= win and vim.api.nvim_win_get_buf(other) == viewport then restore_viewport(other) end
    end
    show_viewport(win)
  end
  publish()
  if forms then show_forms() end
end, {desc = "Toggle the 3D viewport in this window"})
vim.cmd([[cnoreabbrev <expr> viewport getcmdtype() ==# ':' && getcmdline() ==# 'viewport' ? 'Viewport' : 'viewport']])
vim.api.nvim_create_autocmd("WinClosed", {
  group = group, callback = function(event) viewport_views[tonumber(event.match)] = nil end,
})
vim.api.nvim_create_autocmd("CmdlineLeave", {
  group = group,
  callback = function()
    if vim.api.nvim_get_current_buf() ~= viewport then return end
    vim.schedule(function()
      if vim.api.nvim_get_current_buf() == viewport then
        publish()
        vim.rpcnotify(channel, "kalam.viewport_command_done")
      end
    end)
  end,
})
local original = vim.api.nvim_get_current_win()
if gui_workspace then
  vim.api.nvim_create_autocmd("User", {
    group = group, pattern = "KalamGuiDocument", nested = true,
    callback = function(event)
      local id = event.data.id
      if id == "" then
        switch_buffer(vim.api.nvim_get_current_win(), source)
      elseif valid_entity_id(id) then
        local file = entity_path(id)
        if vim.fn.filereadable(file) ~= 1 then return end
        local buffer = vim.fn.bufadd(file)
        vim.fn.bufload(buffer)
        vim.bo[buffer].buflisted = true
        switch_buffer(vim.api.nvim_get_current_win(), buffer)
      end
    end,
  })
  set_mode("gui", original)
else
  open()
  vim.api.nvim_set_current_win(original)
end

vim.api.nvim_create_autocmd("User", {
  -- Let bufload trigger filetype detection when picking opens an entity file.
  group = group, pattern = "KalamViewportPicked", nested = true,
  callback = function(event)
    local id = event.data.id
    if not valid_entity_id(id) then return end
    local file = entity_path(id)
    if vim.fn.filereadable(file) ~= 1 then return end
    local buffer = vim.fn.bufadd(file)
    vim.fn.bufload(buffer)
    vim.bo[buffer].buflisted = true
    local target = vim.fn.bufwinid(buffer)
    if target ~= -1 then return end
    local win = vim.fn.bufwinid(source)
    if win == -1 then
      for _, other in ipairs(vim.api.nvim_tabpage_list_wins(0)) do
        local current = vim.api.nvim_win_get_buf(other)
        if is_json(current) and vim.fs.dirname(vim.api.nvim_buf_get_name(current)) == entity_dir then
          win = other
          break
        end
      end
    end
    if win == -1 then
      vim.api.nvim_win_call(0, function() vim.cmd("leftabove vertical sbuffer " .. buffer) end)
    else
      switch_buffer(win, buffer)
    end
  end,
})
