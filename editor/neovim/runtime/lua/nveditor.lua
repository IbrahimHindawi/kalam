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

local context_sessions = {}
local context_session_by_buffer = {}
local next_context_session_id = 1

local function context_field_line(session, index, value_json)
  local suffix = index < #session.fields and "," or ""
  return "  " .. vim.json.encode(session.fields[index].name)
    .. ": " .. value_json .. suffix
end

local function context_session_lines(session)
  local lines = { "{" }
  for index, field in ipairs(session.fields) do
    table.insert(lines, context_field_line(session, index, field.value_json))
  end
  table.insert(lines, "}")
  return lines
end

local function context_window_geometry(window)
  if not window or not vim.api.nvim_win_is_valid(window) then
    return 0, 0, 0, 0
  end
  local position = vim.api.nvim_win_get_position(window)
  return position[1], position[2], vim.api.nvim_win_get_width(window),
    vim.api.nvim_win_get_height(window)
end

local function context_session_payload(session)
  local row, column, width, height = context_window_geometry(session.window)
  local cursor = { 1, 0 }
  if session.window and vim.api.nvim_win_is_valid(session.window) then
    cursor = vim.api.nvim_win_get_cursor(session.window)
  end
  local modified = false
  local changedtick = 0
  local text = ""
  if session.buffer and vim.api.nvim_buf_is_valid(session.buffer) then
    modified = vim.bo[session.buffer].modified
    changedtick = vim.api.nvim_buf_get_changedtick(session.buffer)
    text = text_view(session.buffer)
  end
  return {
    session_id = session.id,
    bufnr = session.buffer,
    window = session.window,
    changedtick = changedtick,
    text = text,
    mode = vim.api.nvim_get_mode().mode,
    cursor_row = cursor[1] - 1,
    cursor_column = cursor[2],
    window_row = row,
    window_column = column,
    window_width = width,
    window_height = height,
    modified = modified,
    write_pending = session.write_pending == true,
    json_path = session.json_path,
    source_buffer = session.source_buffer,
  }
end

local function apply_context_statusline(session)
  if session.window and vim.api.nvim_win_is_valid(session.window) then
    vim.o.laststatus = 2
    vim.wo[session.window].statusline = session.statusline
  end
end

local function publish_context_session(session)
  if context_sessions[session.id] ~= session
      or not vim.api.nvim_buf_is_valid(session.buffer) then
    return
  end
  rpc_notify("nveditor.context_session_changed", context_session_payload(session))
end

local function validate_context_session(session)
  local validation = M.validate_text(text_view(session.buffer))
  if not validation.ok then
    local diagnostic = validation.diagnostics[1] or {}
    return nil, diagnostic.message or "invalid context JSON"
  end
  if type(validation.decoded) ~= "table" then
    return nil, "the context buffer must contain a JSON object"
  end

  local expected = {}
  for _, field in ipairs(session.fields) do
    expected[field.name] = true
    if validation.decoded[field.name] == nil then
      return nil, "missing context field: " .. field.name
    end
  end
  for name, _ in pairs(validation.decoded) do
    if type(name) ~= "string" or not expected[name] then
      return nil, "unexpected context field: " .. tostring(name)
    end
  end
  return validation.decoded
end

local function begin_context_session_write(session)
  if context_sessions[session.id] ~= session
      or not vim.api.nvim_buf_is_valid(session.buffer) then
    return nil, "context session is no longer open"
  end
  if session.write_pending then
    return nil, "context write is already in progress"
  end

  local _, validation_error = validate_context_session(session)
  if validation_error then
    return nil, validation_error
  end

  session.write_pending = true
  vim.bo[session.buffer].modified = false
  vim.bo[session.buffer].modifiable = false
  rpc_notify(
    "nveditor.context_session_write",
    context_session_payload(session)
  )
  vim.schedule(function()
    publish_context_session(session)
  end)
  return true
end

local function forget_context_session(session)
  if context_sessions[session.id] ~= session then
    return
  end
  context_sessions[session.id] = nil
  context_session_by_buffer[session.buffer] = nil
  if next(context_sessions) == nil then
    if session.paused_lualine and type(session.lualine) == "table"
        and type(session.lualine.hide) == "function" then
      pcall(session.lualine.hide, {
        place = { "statusline" },
        unhide = true,
      })
    end
    if session.old_laststatus ~= nil then
      vim.o.laststatus = session.old_laststatus
    end
  end
  rpc_notify("nveditor.context_session_closed", {
    session_id = session.id,
    bufnr = session.buffer,
    write_pending = session.write_pending == true,
    json_path = session.json_path,
    source_buffer = session.source_buffer,
  })
end

function M.open_context_session(spec)
  if type(spec) ~= "table" or type(spec.fields) ~= "table"
      or #spec.fields == 0 then
    return nil, "context session requires at least one field"
  end

  local fields = {}
  local seen = {}
  for index, field in ipairs(spec.fields) do
    if type(field) ~= "table" or type(field.name) ~= "string"
        or field.name == "" or type(field.value_json) ~= "string" then
      return nil, "invalid context field at index " .. index
    end
    if seen[field.name] then
      return nil, "duplicate context field: " .. field.name
    end
    local ok = pcall(vim.json.decode, field.value_json)
    if not ok then
      return nil, "invalid JSON value for context field: " .. field.name
    end
    seen[field.name] = true
    table.insert(fields, {
      name = field.name,
      value_json = field.value_json,
    })
  end

  local origin_window = vim.api.nvim_get_current_win()
  local origin_tab = vim.api.nvim_get_current_tabpage()
  local old_laststatus = vim.o.laststatus
  local lualine = package.loaded.lualine
  local paused_lualine = false
  if type(lualine) == "table" and type(lualine.hide) == "function" then
    paused_lualine = pcall(lualine.hide, {
      place = { "statusline" },
      unhide = false,
    })
  end
  vim.cmd("tabnew")
  local window = vim.api.nvim_get_current_win()
  local tabpage = vim.api.nvim_get_current_tabpage()
  local buffer = vim.api.nvim_get_current_buf()
  local session_id = next_context_session_id
  next_context_session_id = next_context_session_id + 1
  local session = {
    id = session_id,
    buffer = buffer,
    window = window,
    tabpage = tabpage,
    origin_window = origin_window,
    origin_tab = origin_tab,
    source_buffer = tonumber(spec.source_buffer) or 0,
    json_path = tostring(spec.json_path or "$"),
    fields = fields,
    write_pending = false,
    old_laststatus = old_laststatus,
    paused_lualine = paused_lualine,
    lualine = lualine,
  }
  context_sessions[session_id] = session
  context_session_by_buffer[buffer] = session

  vim.api.nvim_buf_set_name(
    buffer,
    string.format("kalam://context/%d", session_id)
  )
  vim.bo[buffer].buftype = "acwrite"
  vim.bo[buffer].bufhidden = "wipe"
  vim.bo[buffer].buflisted = false
  vim.bo[buffer].swapfile = false
  vim.bo[buffer].filetype = "json"
  vim.bo[buffer].modifiable = true
  vim.api.nvim_buf_set_lines(buffer, 0, -1, true, context_session_lines(session))
  vim.bo[buffer].modified = false
  -- Treat the generated staging document like file-loaded text. This closes
  -- its seed undo block so the first keyboard or mouse edit is independently
  -- undoable instead of undoing the entire generated buffer.
  vim.api.nvim_buf_call(buffer, function()
    vim.cmd("silent undo")
    vim.cmd("silent redo")
  end)
  vim.bo[buffer].modified = false

  local status_path = session.json_path:gsub("%%", "%%%%")
  session.statusline = table.concat({
    "%#StatusLine# Kalam context: ",
    status_path,
    " %m %= :w apply  :q cancel  :q! discard  :wq apply+close ",
  })
  apply_context_statusline(session)
  if #fields > 0 then
    vim.api.nvim_win_set_cursor(window, { 2, 0 })
  end
  vim.cmd("stopinsert")

  local group = vim.api.nvim_create_augroup(
    "KalamContextSession" .. session_id,
    { clear = true }
  )
  vim.api.nvim_create_autocmd(
    { "TextChanged", "TextChangedI", "CursorMoved", "CursorMovedI", "ModeChanged", "BufEnter", "WinEnter" },
    {
      group = group,
      buffer = buffer,
      callback = function()
        apply_context_statusline(session)
        vim.schedule(function()
          publish_context_session(session)
        end)
      end,
    }
  )
  vim.api.nvim_create_autocmd("BufWriteCmd", {
    group = group,
    buffer = buffer,
    callback = function()
      local ok, write_error = begin_context_session_write(session)
      if not ok then
        error("Kalam context: " .. write_error, 0)
      end
    end,
  })
  vim.api.nvim_create_autocmd("BufWipeout", {
    group = group,
    buffer = buffer,
    once = true,
    callback = function()
      forget_context_session(session)
    end,
  })

  vim.schedule(function()
    publish_context_session(session)
  end)
  return context_session_payload(session)
end

function M.context_session(session_id)
  local session = context_sessions[tonumber(session_id)]
  if not session then
    return nil, "context session is not open"
  end
  return context_session_payload(session)
end

function M.update_context_session_field(session_id, name, value_json)
  local session = context_sessions[tonumber(session_id)]
  if not session or not vim.api.nvim_buf_is_valid(session.buffer) then
    return nil, "context session is not open"
  end
  if session.write_pending then
    return nil, "context write is in progress"
  end
  if type(name) ~= "string" or type(value_json) ~= "string" then
    return nil, "invalid context field update"
  end
  local ok = pcall(vim.json.decode, value_json)
  if not ok then
    return nil, "context field update is not valid JSON"
  end

  local field_index = nil
  for index, field in ipairs(session.fields) do
    if field.name == name then
      field_index = index
      break
    end
  end
  if not field_index then
    return nil, "context field does not exist: " .. name
  end

  local field = session.fields[field_index]
  field.value_json = value_json
  vim.api.nvim_buf_set_lines(
    session.buffer,
    field_index,
    field_index + 1,
    true,
    { context_field_line(session, field_index, value_json) }
  )
  if vim.api.nvim_win_is_valid(session.window) then
    vim.api.nvim_set_current_win(session.window)
    local prefix = "  " .. vim.json.encode(name) .. ": "
    vim.api.nvim_win_set_cursor(
      session.window,
      { field_index + 1, #prefix }
    )
  end
  publish_context_session(session)
  return context_session_payload(session)
end

function M.finish_context_session_write(session_id, succeeded, message)
  local session = context_sessions[tonumber(session_id)]
  if not session then
    return true
  end
  if not vim.api.nvim_buf_is_valid(session.buffer) then
    forget_context_session(session)
    return true
  end

  vim.bo[session.buffer].modifiable = true
  session.write_pending = false
  vim.bo[session.buffer].modified = not succeeded
  if message and message ~= "" then
    vim.notify(
      message,
      succeeded and vim.log.levels.INFO or vim.log.levels.ERROR
    )
  elseif succeeded then
    vim.notify("Kalam context applied", vim.log.levels.INFO)
  end
  publish_context_session(session)
  return true
end

function M.request_context_session_write(session_id, close_after_write)
  local session = context_sessions[tonumber(session_id)]
  if not session then
    return nil, "context session is not open"
  end
  local ok, write_error = begin_context_session_write(session)
  if not ok then
    return nil, write_error
  end
  if close_after_write then
    vim.schedule(function()
      if context_sessions[session.id] == session
          and vim.api.nvim_win_is_valid(session.window) then
        vim.api.nvim_win_close(session.window, true)
      end
    end)
  end
  return true
end

function M.close_context_session(session_id, force)
  local session = context_sessions[tonumber(session_id)]
  if not session then
    return true
  end
  if vim.api.nvim_win_is_valid(session.window) then
    local ok, close_error = pcall(
      vim.api.nvim_win_close,
      session.window,
      force == true
    )
    if not ok then
      return nil, tostring(close_error)
    end
  elseif vim.api.nvim_buf_is_valid(session.buffer) then
    local ok, close_error = pcall(
      vim.api.nvim_buf_delete,
      session.buffer,
      { force = force == true }
    )
    if not ok then
      return nil, tostring(close_error)
    end
  else
    forget_context_session(session)
  end
  return true
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
  local context, context_error = M.cursor_context()
  if not context then
    vim.notify(context_error, vim.log.levels.WARN)
    return nil, context_error
  end
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

action_registry["edit.context"].run = dispatch_edit_context
action_registry["edit.form"].run = dispatch_edit_form
action_registry["build.reload"].run = dispatch_build_reload
action_registry["focus.editor"].run = dispatch_focus
action_registry["focus.viewport"].run = dispatch_focus
action_registry["toggle.viewport"].run = dispatch_simple
action_registry.play.run = dispatch_simple
action_registry.stop.run = dispatch_simple

local function notify_help(prefix)
  local lines = { "Kalam actions:" }
  local found = false
  for _, action_id in ipairs(action_order) do
    local action = action_registry[action_id]
    local syntax = action_syntax(action)
    if prefix == nil or syntax:sub(1, #prefix) == prefix then
      table.insert(lines, string.format("  :Kalam %-16s %s", syntax, action.description))
      found = true
    end
  end

  if not found then
    local message = string.format(
      "unknown Kalam operation: %s (run :Kalam for help)",
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
        string.format("  %s  -> :Kalam %s", plug_mappings[action_id], syntax)
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
    local message = string.format("unknown Kalam action: %s", tostring(action_id))
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
    "unknown Kalam operation: %s (run :Kalam for help)",
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
      desc = "Kalam Neovim-native editor actions",
    }
  )

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
end

return M
