local M = {}

local debounce_generation = {}
local setup_complete = false

local function text_view(bufnr)
  local lines = vim.api.nvim_buf_get_lines(bufnr, 0, -1, true)
  return table.concat(lines, "\n")
end

local function path_component(path, key)
  if key:match("^[A-Za-z_$][A-Za-z0-9_$]*$") then
    return path .. "." .. key
  end
  return path .. "[" .. vim.json.encode(key) .. "]"
end

local function offset_location(text, target)
  local row = 0
  local column = 0
  local index = 1
  while index < target do
    local byte = text:byte(index)
    if byte == 10 then
      row = row + 1
      column = 0
    else
      column = column + 1
    end
    index = index + 1
  end
  return row, column
end

local function duplicate_key_scan(text)
  local cursor = 1
  local size = #text

  local function skip_whitespace()
    while cursor <= size do
      local byte = text:byte(cursor)
      if byte ~= 32 and byte ~= 9 and byte ~= 10 and byte ~= 13 then
        break
      end
      cursor = cursor + 1
    end
  end

  local function parse_string()
    skip_whitespace()
    if text:byte(cursor) ~= 34 then
      return nil, "expected JSON string", cursor
    end

    local first = cursor
    cursor = cursor + 1
    while cursor <= size do
      local byte = text:byte(cursor)
      if byte == 34 then
        cursor = cursor + 1
        local raw = text:sub(first, cursor - 1)
        local ok, decoded = pcall(vim.json.decode, raw)
        if not ok or type(decoded) ~= "string" then
          return nil, "invalid JSON string", first
        end
        return decoded, nil, first
      end
      if byte == 92 then
        cursor = cursor + 1
        if cursor > size then
          return nil, "incomplete JSON escape", first
        end
        if text:byte(cursor) == 117 then
          cursor = cursor + 4
          if cursor > size then
            return nil, "incomplete Unicode escape", first
          end
        end
      elseif byte < 32 then
        return nil, "control byte in JSON string", cursor
      end
      cursor = cursor + 1
    end
    return nil, "unterminated JSON string", first
  end

  local parse_value

  local function parse_object(path)
    local seen = {}
    cursor = cursor + 1
    skip_whitespace()
    if text:byte(cursor) == 125 then
      cursor = cursor + 1
      return nil
    end

    while cursor <= size do
      local key, string_error, key_offset = parse_string()
      if string_error then
        return {
          message = string_error,
          path = path,
          offset = key_offset,
        }
      end

      local field_path = path_component(path, key)
      if seen[key] then
        return {
          message = "duplicate JSON object key",
          path = field_path,
          offset = key_offset,
        }
      end
      seen[key] = true

      skip_whitespace()
      if text:byte(cursor) ~= 58 then
        return {
          message = "expected ':' after JSON object key",
          path = field_path,
          offset = cursor,
        }
      end
      cursor = cursor + 1

      local value_error = parse_value(field_path)
      if value_error then
        return value_error
      end

      skip_whitespace()
      local byte = text:byte(cursor)
      if byte == 125 then
        cursor = cursor + 1
        return nil
      end
      if byte ~= 44 then
        return {
          message = "expected ',' or '}' in JSON object",
          path = path,
          offset = cursor,
        }
      end
      cursor = cursor + 1
    end

    return {
      message = "unterminated JSON object",
      path = path,
      offset = cursor,
    }
  end

  local function parse_array(path)
    local element_index = 0
    cursor = cursor + 1
    skip_whitespace()
    if text:byte(cursor) == 93 then
      cursor = cursor + 1
      return nil
    end

    while cursor <= size do
      local value_error = parse_value(path .. "[" .. element_index .. "]")
      if value_error then
        return value_error
      end
      element_index = element_index + 1

      skip_whitespace()
      local byte = text:byte(cursor)
      if byte == 93 then
        cursor = cursor + 1
        return nil
      end
      if byte ~= 44 then
        return {
          message = "expected ',' or ']' in JSON array",
          path = path,
          offset = cursor,
        }
      end
      cursor = cursor + 1
    end

    return {
      message = "unterminated JSON array",
      path = path,
      offset = cursor,
    }
  end

  function parse_value(path)
    skip_whitespace()
    local byte = text:byte(cursor)
    if byte == 123 then
      return parse_object(path)
    end
    if byte == 91 then
      return parse_array(path)
    end
    if byte == 34 then
      local _, string_error, offset = parse_string()
      if string_error then
        return {
          message = string_error,
          path = path,
          offset = offset,
        }
      end
      return nil
    end

    if text:sub(cursor, cursor + 3) == "true"
        or text:sub(cursor, cursor + 3) == "null" then
      cursor = cursor + 4
      return nil
    end
    if text:sub(cursor, cursor + 4) == "false" then
      cursor = cursor + 5
      return nil
    end

    local start = cursor
    if byte == 45 then
      cursor = cursor + 1
      byte = text:byte(cursor)
    end

    if byte == 48 then
      cursor = cursor + 1
      local next_byte = text:byte(cursor)
      if next_byte and next_byte >= 48 and next_byte <= 57 then
        return {
          message = "leading zero in JSON number",
          path = path,
          offset = start,
        }
      end
    elseif byte and byte >= 49 and byte <= 57 then
      repeat
        cursor = cursor + 1
        byte = text:byte(cursor)
      until not byte or byte < 48 or byte > 57
    else
      return {
        message = "expected JSON value",
        path = path,
        offset = start,
      }
    end

    if text:byte(cursor) == 46 then
      cursor = cursor + 1
      byte = text:byte(cursor)
      if not byte or byte < 48 or byte > 57 then
        return {
          message = "invalid JSON fraction",
          path = path,
          offset = start,
        }
      end
      repeat
        cursor = cursor + 1
        byte = text:byte(cursor)
      until not byte or byte < 48 or byte > 57
    end

    byte = text:byte(cursor)
    if byte == 69 or byte == 101 then
      cursor = cursor + 1
      byte = text:byte(cursor)
      if byte == 43 or byte == 45 then
        cursor = cursor + 1
        byte = text:byte(cursor)
      end
      if not byte or byte < 48 or byte > 57 then
        return {
          message = "invalid JSON exponent",
          path = path,
          offset = start,
        }
      end
      repeat
        cursor = cursor + 1
        byte = text:byte(cursor)
      until not byte or byte < 48 or byte > 57
    end

    byte = text:byte(cursor)
    if byte and byte ~= 32 and byte ~= 9 and byte ~= 10 and byte ~= 13
        and byte ~= 44 and byte ~= 93 and byte ~= 125 then
      return {
        message = "invalid byte after JSON number",
        path = path,
        offset = cursor,
      }
    end
    return nil
  end

  skip_whitespace()
  local scan_error = parse_value("$")
  if scan_error then
    local row, column = offset_location(text, scan_error.offset)
    scan_error.row = row
    scan_error.column = column
    scan_error.offset = nil
    return scan_error
  end
  skip_whitespace()
  if cursor <= size then
    local row, column = offset_location(text, cursor)
    return {
      message = "trailing data after JSON document",
      path = "$",
      row = row,
      column = column,
    }
  end
  return nil
end

function M.validate_text(text)
  if type(text) ~= "string" then
    return {
      ok = false,
      diagnostics = {
        {
          message = "JSON text must be a string",
          path = "$",
          row = 0,
          column = 0,
        },
      },
    }
  end

  local decode_ok, decoded = pcall(vim.json.decode, text)
  if not decode_ok then
    return {
      ok = false,
      diagnostics = {
        {
          message = tostring(decoded),
          path = "$",
          row = 0,
          column = 0,
        },
      },
    }
  end

  local duplicate_error = duplicate_key_scan(text)
  if duplicate_error then
    return {
      ok = false,
      diagnostics = { duplicate_error },
    }
  end

  return {
    ok = true,
    decoded = decoded,
    diagnostics = {},
  }
end

function M.get_active_buffer()
  local bufnr = vim.api.nvim_get_current_buf()
  return {
    bufnr = bufnr,
    changedtick = vim.api.nvim_buf_get_changedtick(bufnr),
    path = vim.api.nvim_buf_get_name(bufnr),
    filetype = vim.bo[bufnr].filetype,
  }
end

function M.get_buffer_text(bufnr)
  bufnr = bufnr or vim.api.nvim_get_current_buf()
  if not vim.api.nvim_buf_is_valid(bufnr) then
    return nil, "invalid Neovim buffer"
  end
  return {
    bufnr = bufnr,
    changedtick = vim.api.nvim_buf_get_changedtick(bufnr),
    text = text_view(bufnr),
  }
end

function M.validate_buffer(bufnr)
  bufnr = bufnr or vim.api.nvim_get_current_buf()
  local snapshot, snapshot_error = M.get_buffer_text(bufnr)
  if not snapshot then
    return {
      ok = false,
      diagnostics = {
        {
          message = snapshot_error,
          path = "$",
          row = 0,
          column = 0,
        },
      },
    }
  end

  local result = M.validate_text(snapshot.text)
  result.bufnr = bufnr
  result.changedtick = snapshot.changedtick
  result.text = snapshot.text
  result.path = vim.api.nvim_buf_get_name(bufnr)
  return result
end

local function tree_sitter_root(bufnr)
  local ok, parser = pcall(vim.treesitter.get_parser, bufnr, "json")
  if not ok or not parser then
    return nil,
      "JSON Tree-sitter parser unavailable; contextual editing is disabled"
  end

  local trees = parser:parse()
  if not trees or not trees[1] then
    return nil, "JSON Tree-sitter produced no syntax tree"
  end
  return trees[1]:root()
end

local function node_text(node, bufnr)
  local ok, value = pcall(vim.treesitter.get_node_text, node, bufnr)
  if not ok then
    return nil
  end
  return value
end

local function pair_value_node(pair)
  local values = pair:field("value")
  if values and values[1] then
    return values[1]
  end
  local child_count = pair:named_child_count()
  if child_count >= 2 then
    return pair:named_child(1)
  end
  return nil
end

local function pair_key(pair, bufnr)
  local keys = pair:field("key")
  local key_node = keys and keys[1] or pair:named_child(0)
  if not key_node then
    return nil
  end
  local raw = node_text(key_node, bufnr)
  local ok, decoded = pcall(vim.json.decode, raw)
  if ok and type(decoded) == "string" then
    return decoded
  end
  return nil
end

local function containing_pair(node)
  local current = node
  while current do
    if current:type() == "pair" then
      return current
    end
    current = current:parent()
  end
  return nil
end

local function pair_path(pair, bufnr)
  local keys = {}
  local current = pair
  while current do
    if current:type() == "pair" then
      local key = pair_key(current, bufnr)
      if not key then
        return nil
      end
      table.insert(keys, 1, key)
    end
    current = current:parent()
  end

  local path = "$"
  for _, key in ipairs(keys) do
    path = path_component(path, key)
  end
  return path
end

local function source_context(bufnr, json_path, value)
  local start_row, start_column, end_row, end_column = value:range()
  return {
    bufnr = bufnr,
    changedtick = vim.api.nvim_buf_get_changedtick(bufnr),
    json_path = json_path,
    node_type = value:type(),
    start_row = start_row,
    start_column = start_column,
    end_row = end_row,
    end_column = end_column,
    source_text = node_text(value, bufnr),
    path = vim.api.nvim_buf_get_name(bufnr),
    text = text_view(bufnr),
  }
end

local fallback_paths = {
  ["$"] = true,
  ["$.cube"] = true,
  ["$.cube.color"] = true,
  ["$.cube.transform"] = true,
  ["$.cube.transform.translation"] = true,
  ["$.cube.transform.rotation_degrees"] = true,
  ["$.cube.transform.scale"] = true,
  ["$.exposure"] = true,
  ["$.bloom_threshold"] = true,
  ["$.vignette"] = true,
  ["$.tint"] = true,
}

local function fallback_source_index(text)
  local cursor = 1
  local size = #text
  local entries = {}

  local function skip_whitespace()
    while cursor <= size do
      local byte = text:byte(cursor)
      if byte ~= 32 and byte ~= 9 and byte ~= 10 and byte ~= 13 then
        break
      end
      cursor = cursor + 1
    end
  end

  local function parse_string()
    skip_whitespace()
    local start_offset = cursor
    if text:byte(cursor) ~= 34 then
      return nil, "expected JSON string"
    end
    cursor = cursor + 1
    while cursor <= size do
      local byte = text:byte(cursor)
      if byte == 34 then
        cursor = cursor + 1
        local raw = text:sub(start_offset, cursor - 1)
        local ok, decoded = pcall(vim.json.decode, raw)
        if not ok or type(decoded) ~= "string" then
          return nil, "invalid JSON string"
        end
        return decoded, nil, start_offset
      end
      if byte == 92 then
        cursor = cursor + 1
        if text:byte(cursor) == 117 then
          cursor = cursor + 4
        end
      end
      cursor = cursor + 1
    end
    return nil, "unterminated JSON string"
  end

  local parse_value

  local function parse_object(path)
    cursor = cursor + 1
    skip_whitespace()
    if text:byte(cursor) == 125 then
      cursor = cursor + 1
      return nil
    end
    while cursor <= size do
      local key, key_error, key_start = parse_string()
      if key_error then
        return key_error
      end
      local child_path = path_component(path, key)
      skip_whitespace()
      if text:byte(cursor) ~= 58 then
        return "expected ':' after JSON object key"
      end
      cursor = cursor + 1
      local value_error = parse_value(child_path, key_start)
      if value_error then
        return value_error
      end
      skip_whitespace()
      local byte = text:byte(cursor)
      if byte == 125 then
        cursor = cursor + 1
        return nil
      end
      if byte ~= 44 then
        return "expected ',' or '}' in JSON object"
      end
      cursor = cursor + 1
    end
    return "unterminated JSON object"
  end

  local function parse_array(path)
    local element_index = 0
    cursor = cursor + 1
    skip_whitespace()
    if text:byte(cursor) == 93 then
      cursor = cursor + 1
      return nil
    end
    while cursor <= size do
      local value_error = parse_value(path .. "[" .. element_index .. "]")
      if value_error then
        return value_error
      end
      element_index = element_index + 1
      skip_whitespace()
      local byte = text:byte(cursor)
      if byte == 93 then
        cursor = cursor + 1
        return nil
      end
      if byte ~= 44 then
        return "expected ',' or ']' in JSON array"
      end
      cursor = cursor + 1
    end
    return "unterminated JSON array"
  end

  function parse_value(path, pair_start)
    skip_whitespace()
    local start_offset = cursor
    local byte = text:byte(cursor)
    local node_type
    local parse_error
    if byte == 123 then
      node_type = "object"
      parse_error = parse_object(path)
    elseif byte == 91 then
      node_type = "array"
      parse_error = parse_array(path)
    elseif byte == 34 then
      node_type = "string"
      local _, string_error = parse_string()
      parse_error = string_error
    elseif text:sub(cursor, cursor + 3) == "true" then
      node_type = "true"
      cursor = cursor + 4
    elseif text:sub(cursor, cursor + 4) == "false" then
      node_type = "false"
      cursor = cursor + 5
    elseif text:sub(cursor, cursor + 3) == "null" then
      node_type = "null"
      cursor = cursor + 4
    else
      node_type = "number"
      while cursor <= size do
        byte = text:byte(cursor)
        if byte == 32 or byte == 9 or byte == 10 or byte == 13
            or byte == 44 or byte == 93 or byte == 125 then
          break
        end
        cursor = cursor + 1
      end
      if cursor == start_offset then
        parse_error = "expected JSON value"
      end
    end
    if parse_error then
      return parse_error
    end
    entries[path] = {
      path = path,
      start_offset = start_offset,
      end_offset = cursor,
      pair_start = pair_start or start_offset,
      node_type = path == "$" and "document" or node_type,
    }
    return nil
  end

  local parse_error = parse_value("$")
  if parse_error then
    return nil, parse_error
  end
  skip_whitespace()
  if cursor <= size then
    return nil, "trailing data after JSON document"
  end
  return entries
end

local function fallback_context_from_entry(bufnr, text, entry)
  local start_row, start_column = offset_location(text, entry.start_offset)
  local end_row, end_column = offset_location(text, entry.end_offset)
  return {
    bufnr = bufnr,
    changedtick = vim.api.nvim_buf_get_changedtick(bufnr),
    json_path = entry.path,
    node_type = entry.node_type,
    start_row = start_row,
    start_column = start_column,
    end_row = end_row,
    end_column = end_column,
    source_text = text:sub(entry.start_offset, entry.end_offset - 1),
    path = vim.api.nvim_buf_get_name(bufnr),
    text = text,
    range_fallback = true,
  }
end

local function fallback_context_for_path(bufnr, wanted_path, text)
  if not fallback_paths[wanted_path] then
    return nil,
      "JSON Tree-sitter parser unavailable; fallback supports only fixed schemas"
  end
  local entries, index_error = fallback_source_index(text)
  if not entries then
    return nil, "fallback JSON source index failed: " .. index_error
  end
  local entry = entries[wanted_path]
  if not entry then
    return nil, "JSON path is not present in the active buffer"
  end
  return fallback_context_from_entry(bufnr, text, entry)
end

local function byte_offset_for_location(text, wanted_row, wanted_column)
  local row = 0
  local line_start = 1
  for offset = 1, #text + 1 do
    if row == wanted_row then
      return math.min(line_start + wanted_column, #text + 1)
    end
    if text:byte(offset) == 10 then
      row = row + 1
      line_start = offset + 1
    end
  end
  return #text + 1
end

local function fallback_context_at(bufnr, row, column, text)
  local entries, index_error = fallback_source_index(text)
  if not entries then
    return nil, "fallback JSON source index failed: " .. index_error
  end
  local offset = byte_offset_for_location(text, row, column)
  local best
  for path, entry in pairs(entries) do
    if fallback_paths[path]
        and entry.pair_start <= offset
        and offset <= entry.end_offset then
      if not best
          or (entry.end_offset - entry.pair_start)
              < (best.end_offset - best.pair_start) then
        best = entry
      end
    end
  end
  if not best then
    return nil, "cursor is not inside a supported fixed-schema JSON field"
  end
  return fallback_context_from_entry(bufnr, text, best)
end

function M.context_for_path(bufnr, wanted_path)
  bufnr = bufnr or vim.api.nvim_get_current_buf()
  if not vim.api.nvim_buf_is_valid(bufnr) then
    return nil, "invalid Neovim buffer"
  end
  if type(wanted_path) ~= "string" or wanted_path:sub(1, 1) ~= "$" then
    return nil, "invalid JSON path"
  end

  local validation = M.validate_buffer(bufnr)
  if not validation.ok then
    return nil, "active JSON buffer is invalid"
  end

  if vim.g.nveditor_force_range_fallback == 1 then
    return fallback_context_for_path(bufnr, wanted_path, validation.text)
  end

  local root, root_error = tree_sitter_root(bufnr)
  if not root then
    if root_error:match("parser unavailable") then
      return fallback_context_for_path(bufnr, wanted_path, validation.text)
    end
    return nil, root_error
  end
  if wanted_path == "$" then
    return source_context(bufnr, wanted_path, root)
  end

  local function visit(node)
    if node:type() == "pair" then
      local path = pair_path(node, bufnr)
      if path == wanted_path then
        return pair_value_node(node)
      end
    end

    for child_index = 0, node:named_child_count() - 1 do
      local found = visit(node:named_child(child_index))
      if found then
        return found
      end
    end
    return nil
  end

  local value = visit(root)
  if not value then
    return nil, "JSON path is not present in the active buffer"
  end
  return source_context(bufnr, wanted_path, value)
end

function M.cursor_context(bufnr, row, column)
  bufnr = bufnr or vim.api.nvim_get_current_buf()
  if not vim.api.nvim_buf_is_valid(bufnr) then
    return nil, "invalid Neovim buffer"
  end

  local validation = M.validate_buffer(bufnr)
  if not validation.ok then
    return nil, "active JSON buffer is invalid"
  end

  if row == nil or column == nil then
    local cursor = vim.api.nvim_win_get_cursor(0)
    row = cursor[1] - 1
    column = cursor[2]
  end

  if vim.g.nveditor_force_range_fallback == 1 then
    return fallback_context_at(bufnr, row, column, validation.text)
  end

  local root, root_error = tree_sitter_root(bufnr)
  if not root then
    if root_error:match("parser unavailable") then
      return fallback_context_at(bufnr, row, column, validation.text)
    end
    return nil, root_error
  end

  local node = root:named_descendant_for_range(row, column, row, column)
  if not node then
    return nil, "no JSON syntax node at cursor"
  end

  local pair = containing_pair(node)
  if not pair then
    return nil, "cursor is not inside a JSON field"
  end
  local value = pair_value_node(pair)
  if not value then
    return nil, "JSON field has no value node"
  end

  local json_path = pair_path(pair, bufnr)
  if not json_path then
    return nil, "could not resolve JSON path"
  end

  return source_context(bufnr, json_path, value)
end

function M.patch_text(request)
  if type(request) ~= "table"
      or type(request.bufnr) ~= "number"
      or type(request.expected_changedtick) ~= "number"
      or type(request.replacement) ~= "string" then
    return nil, "invalid text-patch request"
  end
  if not vim.api.nvim_buf_is_valid(request.bufnr) then
    return nil, "text-patch buffer is invalid"
  end

  local changedtick = vim.api.nvim_buf_get_changedtick(request.bufnr)
  if changedtick ~= request.expected_changedtick then
    return nil, "buffer changedtick no longer matches transaction"
  end

  local replacement_lines =
    vim.split(request.replacement, "\n", { plain = true })
  local ok, patch_error = pcall(
    vim.api.nvim_buf_set_text,
    request.bufnr,
    request.start_row,
    request.start_column,
    request.end_row,
    request.end_column,
    replacement_lines
  )
  if not ok then
    return nil, tostring(patch_error)
  end

  return {
    bufnr = request.bufnr,
    changedtick = vim.api.nvim_buf_get_changedtick(request.bufnr),
  }
end

function M.set_quickfix(items, title)
  vim.fn.setqflist({}, "r", {
    title = title or "Kalam",
    items = items or {},
  })
end

local function rpc_notify(method, payload)
  local channel = tonumber(vim.g.nveditor_channel_id)
  if channel and channel > 0 then
    local ok = pcall(vim.rpcnotify, channel, method, payload)
    if ok then
      return true
    end
  end

  pcall(vim.api.nvim_exec_autocmds, "User", {
    pattern = "NvEditorEvent",
    modeline = false,
    data = {
      method = method,
      payload = payload,
    },
  })
  return false
end

-- Public Kalam actions use these identifiers everywhere inside the runtime.
-- Their command spelling and their RPC notification are deliberately kept
-- separate: the spelling is user-facing, while the action id remains stable
-- for mappings, Lua configuration, and other Kalam affordances.
local action_order = {
  "edit.context",
  "edit.form",
  "build.reload",
  "focus.editor",
  "focus.viewport",
  "toggle.viewport",
  "play",
  "stop",
}

local action_registry = {
  ["edit.context"] = {
    words = { "edit", "context" },
    description = "Edit the authored JSON field at the cursor",
    notification = "nveditor.context_edit",
  },
  ["edit.form"] = {
    words = { "edit", "form" },
    description = "Open a generated form for the active authored JSON document",
    notification = "nveditor.form_edit",
  },
  ["build.reload"] = {
    words = { "build", "reload" },
    description = "Build and hot-reload the Kalam game module",
    notification = "nveditor.build_reload",
  },
  ["focus.editor"] = {
    words = { "focus", "editor" },
    description = "Move keyboard focus to the Neovim editor",
    notification = "nveditor.focus_editor",
  },
  ["focus.viewport"] = {
    words = { "focus", "viewport" },
    description = "Move keyboard focus to the game viewport",
    notification = "nveditor.focus_viewport",
  },
  ["toggle.viewport"] = {
    words = { "toggle", "viewport" },
    description = "Show or hide the game viewport",
    notification = "nveditor.toggle_viewport",
  },
  ["play"] = {
    words = { "play" },
    description = "Enter viewport navigation capture",
    notification = "nveditor.play",
  },
  ["stop"] = {
    words = { "stop" },
    description = "Leave viewport navigation capture and return to Neovim",
    notification = "nveditor.stop",
  },
}

local action_by_command = {}
for _, action_id in ipairs(action_order) do
  local action = action_registry[action_id]
  action_by_command[table.concat(action.words, " ")] = action_id
end

local plug_mappings = {
  ["edit.context"] = "<Plug>(KalamEditContext)",
  ["edit.form"] = "<Plug>(KalamEditForm)",
  ["build.reload"] = "<Plug>(KalamBuildReload)",
  ["focus.editor"] = "<Plug>(KalamFocusEditor)",
  ["focus.viewport"] = "<Plug>(KalamFocusViewport)",
  ["toggle.viewport"] = "<Plug>(KalamToggleViewport)",
  ["play"] = "<Plug>(KalamPlay)",
  ["stop"] = "<Plug>(KalamStop)",
}

local function action_syntax(action)
  return table.concat(action.words, " ")
end

local function action_payload(action_id, payload)
  payload = payload or {}
  payload.action = action_id
  return payload
end

local function focus_payload(action_id)
  local window = vim.api.nvim_get_current_win()
  return action_payload(action_id, {
    requested_at = vim.uv.hrtime(),
    window = window,
    buffer = vim.api.nvim_get_current_buf(),
    cursor = vim.api.nvim_win_get_cursor(window),
  })
end

local function dispatch_edit_context(action_id, action)
  local window = vim.api.nvim_get_current_win()
  local cursor = vim.api.nvim_win_get_cursor(window)
  local context, context_error = M.cursor_context()
  if not context then
    vim.notify(context_error, vim.log.levels.WARN)
    return nil, context_error
  end
  local screen = vim.fn.screenpos(window, cursor[1], cursor[2] + 1)
  context.screen_row = screen.row > 0 and screen.row - 1 or -1
  context.screen_column = screen.col > 0 and screen.col - 1 or -1
  rpc_notify(action.notification, action_payload(action_id, context))
  return true
end

local function dispatch_edit_form(action_id, action)
  local document = M.validate_buffer()
  if not document.ok then
    local message = "active JSON buffer is invalid"
    vim.notify(message, vim.log.levels.WARN)
    return nil, message
  end
  document.decoded = nil
  rpc_notify(action.notification, action_payload(action_id, document))
  return true
end

local function dispatch_build_reload(action_id, action)
  rpc_notify(action.notification, action_payload(action_id, {
    requested_at = vim.uv.hrtime(),
  }))
  return true
end

local function dispatch_focus(action_id, action)
  rpc_notify(action.notification, focus_payload(action_id))
  return true
end

local function dispatch_simple(action_id, action)
  rpc_notify(action.notification, action_payload(action_id, {
    requested_at = vim.uv.hrtime(),
  }))
  return true
end

-- The buffer-hosted viewport (docs/buffer_hosted_viewport.md section 3).
-- Neovim owns the layout; the Qt shell overlays the engine viewport on
-- whichever window shows this buffer.

local viewport_buffer_name = "kalam://viewport"
local viewport_buffer = nil

local function viewport_windows_in_tab()
  local bufnr = viewport_buffer
  local windows = {}
  if bufnr == nil or not vim.api.nvim_buf_is_valid(bufnr) then
    return windows
  end
  for _, win in ipairs(vim.api.nvim_tabpage_list_wins(0)) do
    if vim.api.nvim_win_get_buf(win) == bufnr then
      table.insert(windows, win)
    end
  end
  return windows
end

local function notify_viewport_windows()
  local windows = viewport_windows_in_tab()
  local active = windows[1] or 0
  local current = vim.api.nvim_get_current_win()
  for _, win in ipairs(windows) do
    if win == current then
      active = win
    end
  end
  rpc_notify("nveditor.viewport_windows", {
    windows = windows,
    active = active,
  })
end

function M.viewport_bufnr()
  if viewport_buffer ~= nil and vim.api.nvim_buf_is_valid(viewport_buffer) then
    return viewport_buffer
  end
  local bufnr = vim.api.nvim_create_buf(false, true)
  vim.api.nvim_buf_set_name(bufnr, viewport_buffer_name)
  vim.api.nvim_buf_set_lines(bufnr, 0, -1, false, {
    "[ kalam viewport - <CR> to enter, q to close ]",
  })
  vim.bo[bufnr].modifiable = false
  vim.bo[bufnr].filetype = "kalam.viewport"
  for _, lhs in ipairs({ "<CR>", "i" }) do
    vim.keymap.set("n", lhs, function()
      return M.dispatch("focus.viewport")
    end, { buffer = bufnr, silent = true, desc = "kalam: enter viewport" })
  end
  vim.keymap.set("n", "q", function()
    local windows = viewport_windows_in_tab()
    if #windows > 0 then
      vim.api.nvim_win_close(windows[1], false)
    end
  end, { buffer = bufnr, silent = true, desc = "kalam: close viewport" })

  local group = vim.api.nvim_create_augroup("KalamViewportBuffer", {
    clear = true,
  })
  vim.api.nvim_create_autocmd({ "BufWinEnter", "BufWinLeave" }, {
    group = group,
    buffer = bufnr,
    callback = function()
      vim.schedule(notify_viewport_windows)
    end,
  })
  vim.api.nvim_create_autocmd("WinClosed", {
    group = group,
    callback = function()
      vim.schedule(notify_viewport_windows)
    end,
  })
  viewport_buffer = bufnr
  return bufnr
end

-- Source to selection (docs/entity_documents.md section 6): the cursor's
-- enclosing top-level object is reported so the viewport can highlight it.
-- Debounced, and only when the enclosing object actually changes.
local cursor_context_last = nil
local cursor_context_generation = 0

-- The enclosing top-level object is what a highlight targets; moving
-- between fields of one object must not re-notify.
function M.cursor_highlight_target(bufnr)
  bufnr = bufnr or vim.api.nvim_get_current_buf()
  if not vim.api.nvim_buf_is_valid(bufnr) then
    return nil
  end
  local name = vim.api.nvim_buf_get_name(bufnr)
  if name == "" or not name:match("%.json$") then
    return nil
  end

  local context = M.cursor_context(bufnr)
  if context == nil or type(context.json_path) ~= "string" then
    return nil
  end
  local enclosing = context.json_path:match("^%$%.[%w_]+") or "$"
  return { path = name, json_path = enclosing }
end

local function notify_cursor_context(bufnr)
  local target = M.cursor_highlight_target(bufnr)
  if target == nil then
    return
  end
  local signature = target.path .. "\0" .. target.json_path
  if signature == cursor_context_last then
    return
  end
  cursor_context_last = signature
  rpc_notify("nveditor.cursor_context", target)
end

local function schedule_cursor_context(bufnr)
  cursor_context_generation = cursor_context_generation + 1
  local generation = cursor_context_generation
  vim.defer_fn(function()
    if generation == cursor_context_generation
        and vim.api.nvim_buf_is_valid(bufnr) then
      notify_cursor_context(bufnr)
    end
  end, tonumber(vim.g.nveditor_cursor_debounce_ms) or 120)
end

-- Opens a project document and places the cursor on the key at json_path
-- (docs/entity_documents.md section 5). A cursor move only: no mode
-- change, no popup. Returns the zero-based row and column it landed on.
function M.open_at_json_path(path, json_path)
  if type(path) ~= "string" or path == "" then
    return nil, "invalid document path"
  end

  -- Prefer a window already showing the file so a viewport pick never
  -- steals the layout the user arranged.
  local absolute = vim.fn.fnamemodify(path, ":p")
  local target_window = nil
  for _, win in ipairs(vim.api.nvim_tabpage_list_wins(0)) do
    local buffer = vim.api.nvim_win_get_buf(win)
    local name = vim.api.nvim_buf_get_name(buffer)
    if name ~= "" and vim.fn.fnamemodify(name, ":p") == absolute then
      target_window = win
      break
    end
  end
  if target_window ~= nil then
    vim.api.nvim_set_current_win(target_window)
  else
    local ok, edit_error = pcall(vim.cmd.edit, vim.fn.fnameescape(path))
    if not ok then
      return nil, tostring(edit_error)
    end
  end

  if type(json_path) ~= "string" or json_path == "" then
    return { row = 0, column = 0 }
  end

  local context = M.context_for_path(0, json_path)
  if context == nil then
    return { row = 0, column = 0 }
  end

  local row = (context.start_row or 0) + 1
  local column = context.start_column or 0
  local line_count = vim.api.nvim_buf_line_count(0)
  if row > line_count then
    row = line_count
  end
  pcall(vim.api.nvim_win_set_cursor, 0, { row, column })
  return { row = context.start_row or 0, column = column }
end

function M.viewport_open()
  local bufnr = M.viewport_bufnr()
  local existing = viewport_windows_in_tab()
  if #existing > 0 then
    return existing[1]
  end
  local previous = vim.api.nvim_get_current_win()
  local width = math.max(20, math.floor(vim.o.columns * 2 / 5))
  vim.cmd("botright " .. width .. "vsplit")
  local win = vim.api.nvim_get_current_win()
  vim.api.nvim_win_set_buf(win, bufnr)
  vim.wo[win].number = false
  vim.wo[win].relativenumber = false
  vim.wo[win].signcolumn = "no"
  vim.wo[win].winfixwidth = false
  if vim.api.nvim_win_is_valid(previous) then
    vim.api.nvim_set_current_win(previous)
  end
  vim.schedule(notify_viewport_windows)
  return win
end

function M.viewport_close()
  local windows = viewport_windows_in_tab()
  for _, win in ipairs(windows) do
    vim.api.nvim_win_close(win, false)
  end
  vim.schedule(notify_viewport_windows)
  return #windows > 0
end

function M.viewport_toggle()
  if #viewport_windows_in_tab() > 0 then
    M.viewport_close()
  else
    M.viewport_open()
  end
  return true
end

local function dispatch_toggle_viewport()
  return M.viewport_toggle()
end

local function dispatch_focus_viewport(action_id, action)
  local windows = viewport_windows_in_tab()
  local win = windows[1] or M.viewport_open()
  if win ~= nil and vim.api.nvim_win_is_valid(win) then
    vim.api.nvim_set_current_win(win)
  end
  rpc_notify(action.notification, focus_payload(action_id))
  return true
end

action_registry["edit.context"].run = dispatch_edit_context
action_registry["edit.form"].run = dispatch_edit_form
action_registry["build.reload"].run = dispatch_build_reload
action_registry["focus.editor"].run = dispatch_focus
action_registry["focus.viewport"].run = dispatch_focus_viewport
action_registry["toggle.viewport"].run = dispatch_toggle_viewport
action_registry.play.run = dispatch_simple
action_registry.stop.run = dispatch_simple

local function notify_help(prefix)
  local lines = { "kalam actions:" }
  local found = false
  for _, action_id in ipairs(action_order) do
    local action = action_registry[action_id]
    local syntax = action_syntax(action)
    if prefix == nil or syntax:sub(1, #prefix) == prefix then
      table.insert(lines, string.format("  :kalam %-16s %s", syntax, action.description))
      found = true
    end
  end

  if not found then
    local message = string.format(
      "unknown kalam operation: %s (run :kalam for help)",
      prefix
    )
    vim.notify(message, vim.log.levels.WARN)
    return nil, message
  end

  table.insert(lines, "")
  table.insert(lines, "No default key mappings are installed. Public <Plug> mappings:")
  for _, action_id in ipairs(action_order) do
    local action = action_registry[action_id]
    local syntax = action_syntax(action)
    if prefix == nil or syntax:sub(1, #prefix) == prefix then
      table.insert(
        lines,
        string.format("  %s  -> :kalam %s", plug_mappings[action_id], syntax)
      )
    end
  end
  vim.notify(table.concat(lines, "\n"), vim.log.levels.INFO)
  return true
end

function M.actions()
  local result = {}
  for _, action_id in ipairs(action_order) do
    local action = action_registry[action_id]
    table.insert(result, {
      id = action_id,
      command = action_syntax(action),
      description = action.description,
      notification = action.notification,
      plug = plug_mappings[action_id],
    })
  end
  return result
end

function M.dispatch(action_id)
  local action = action_registry[action_id]
  if not action then
    local message = string.format("unknown kalam action: %s", tostring(action_id))
    vim.notify(message, vim.log.levels.WARN)
    return nil, message
  end
  return action.run(action_id, action)
end

function M.command(arguments)
  local words = arguments or {}
  if #words == 0 then
    return notify_help()
  end

  if words[1] == "help" then
    if #words == 1 then
      return notify_help()
    end
    return notify_help(table.concat(words, " ", 2))
  end

  local command = table.concat(words, " ")
  local action_id = action_by_command[command]
  if action_id then
    return M.dispatch(action_id)
  end

  for _, action in pairs(action_registry) do
    local syntax = action_syntax(action)
    if syntax:sub(1, #command) == command then
      return notify_help(command)
    end
  end

  local message = string.format(
    "unknown kalam operation: %s (run :kalam for help)",
    command
  )
  vim.notify(message, vim.log.levels.WARN)
  return nil, message
end

local function unique_sorted(values)
  table.sort(values)
  local result = {}
  local previous = nil
  for _, value in ipairs(values) do
    if value ~= previous then
      table.insert(result, value)
      previous = value
    end
  end
  return result
end

function M.complete(arglead, cmdline)
  -- Keep the trailing space: an empty arglead after `:Kalam edit ` means
  -- `edit` is complete and completion must offer its second word.
  local remainder = cmdline:match("^%s*:?[A-Za-z]+%s(.*)$") or ""
  local words = {}
  for word in remainder:gmatch("%S+") do
    table.insert(words, word)
  end

  local prefix = arglead or ""
  if not remainder:match("%s$") and #words > 0 then
    prefix = table.remove(words)
  end

  if words[1] == "help" then
    table.remove(words, 1)
  end

  local values = {}
  for _, action_id in ipairs(action_order) do
    local action_words = action_registry[action_id].words
    local matches = true
    for index, word in ipairs(words) do
      if action_words[index] ~= word then
        matches = false
        break
      end
    end
    local candidate = action_words[#words + 1]
    if matches and candidate and candidate:sub(1, #prefix) == prefix then
      table.insert(values, candidate)
    end
  end

  if #words == 0 and ("help"):sub(1, #prefix) == prefix then
    table.insert(values, "help")
  end
  return unique_sorted(values)
end
-- Compatibility aliases. New mappings and documentation use :Kalam so every
-- invocation reaches the same action dispatcher.
function M.edit_context()
  return M.dispatch("edit.context")
end

function M.edit_form()
  return M.dispatch("edit.form")
end

function M.build_reload()
  return M.dispatch("build.reload")
end

local function schedule_document_validation(bufnr)
  if bufnr ~= vim.api.nvim_get_current_buf() then
    return
  end

  local generation = (debounce_generation[bufnr] or 0) + 1
  debounce_generation[bufnr] = generation
  vim.defer_fn(function()
    if not vim.api.nvim_buf_is_valid(bufnr)
        or debounce_generation[bufnr] ~= generation
        or bufnr ~= vim.api.nvim_get_current_buf() then
      return
    end

    local document = M.validate_buffer(bufnr)
    document.decoded = nil
    rpc_notify(
      document.ok
          and "nveditor.document_valid"
          or "nveditor.document_invalid",
      document
    )
  end, tonumber(vim.g.nveditor_json_debounce_ms) or 150)
end

function M.setup()
  if setup_complete then
    return
  end
  setup_complete = true

  vim.api.nvim_create_user_command(
    "Kalam",
    function(options)
      M.command(options.fargs)
    end,
    {
      nargs = "*",
      complete = function(arglead, cmdline)
        return M.complete(arglead, cmdline)
      end,
      desc = "kalam Neovim-native editor actions",
    }
  )

  -- The registered command is :Kalam only because Neovim requires user
  -- commands to start with an uppercase letter (:help E183). Every
  -- documented invocation is lowercase: this cmdline abbreviation expands
  -- ":kalam" to ":Kalam" when it is the command itself, and never inside
  -- searches, arguments, or mid-line text.
  vim.keymap.set("ca", "kalam", function()
    if vim.fn.getcmdtype() == ":"
        and vim.fn.getcmdline():match("^%s*kalam$") ~= nil then
      return "Kalam"
    end
    return "kalam"
  end, { expr = true, desc = "lowercase :kalam command alias" })

  vim.api.nvim_create_user_command(
    "NvEditContext",
    M.edit_context,
    { desc = "Compatibility alias for :Kalam edit context" }
  )
  vim.api.nvim_create_user_command(
    "NvEditForm",
    M.edit_form,
    { desc = "Compatibility alias for :Kalam edit form" }
  )
  vim.api.nvim_create_user_command(
    "NvBuildReload",
    M.build_reload,
    { desc = "Compatibility alias for :Kalam build reload" }
  )

  -- <Plug> is the only mapping surface Kalam claims. Users decide whether
  -- these actions belong on F-keys, a leader sequence, or no key at all.
  for _, action_id in ipairs(action_order) do
    local action = action_registry[action_id]
    vim.keymap.set(
      "n",
      plug_mappings[action_id],
      "<Cmd>Kalam " .. action_syntax(action) .. "<CR>",
      {
        silent = true,
        desc = "Kalam: " .. action.description,
      }
    )
  end

  local group = vim.api.nvim_create_augroup("KalamNvEditor", { clear = true })
  vim.api.nvim_create_autocmd(
    { "BufEnter", "TextChanged", "TextChangedI" },
    {
      group = group,
      pattern = "*.json",
      callback = function(args)
        schedule_document_validation(args.buf)
      end,
    }
  )
  vim.api.nvim_create_autocmd("BufWipeout", {
    group = group,
    callback = function(args)
      debounce_generation[args.buf] = nil
    end,
  })

  -- Cursor to highlight (docs/entity_documents.md section 6).
  vim.api.nvim_create_autocmd({ "CursorMoved", "CursorHold" }, {
    group = group,
    pattern = "*.json",
    callback = function(args)
      schedule_cursor_context(args.buf)
    end,
  })

  -- Default session layout (docs/buffer_hosted_viewport.md section 5):
  -- one viewport split, opened once, only inside the Qt editor. A user
  -- who closes the window keeps a full-width editor; nothing reopens it
  -- behind their back. Set g:nveditor_auto_viewport = 0 to opt out.
  local channel = tonumber(vim.g.nveditor_channel_id)
  if channel ~= nil and channel > 0
      and vim.g.nveditor_auto_viewport ~= 0 then
    vim.schedule(function()
      if #viewport_windows_in_tab() == 0 then
        M.viewport_open()
      end
    end)
  end
end

return M
