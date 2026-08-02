#!/usr/bin/lua

function file_exists(filename)
  local file = io.open(filename, "rb")
  if file then file:close() end
  return file ~= nil
end

function is_header_file(filename)
  return filename:match("%.[hH]$")
end

function replace_include_guard(filename)
  if not file_exists(filename) or not is_header_file(filename) then
    print(filename .. " skipped: file does not exist or is not a header file")
    return
  end

  local file = io.open(filename, "r")
  local content = {}
  for line in file:lines() do
    table.insert(content, line)
  end
  file:close()

  local ifndef_pattern = "#ifndef ([a-zA-Z_]+_[hH])"
  local ifndef_alt_pattern = "#if !defined%(([a-zA-Z_]+_[hH])%)"
  local define_pattern = "#define ([a-zA-Z_]+_[hH])"

  local ifndef_index, define_index, endif_index = nil, nil, nil
  local ifndef_match, define_match = nil, nil

  for i, line in ipairs(content) do
    if not ifndef_index and line:match(ifndef_pattern) then
      ifndef_index, ifndef_match = i, line:match(ifndef_pattern)
    elseif not ifndef_index and line:match(ifndef_alt_pattern) then
      ifndef_index, ifndef_match = i, line:match(ifndef_alt_pattern)
    elseif ifndef_index and not define_index and line:match(define_pattern) then
      define_index, define_match = i, line:match(define_pattern)
    end
    if line:match("#endif") then
      endif_index = i
    end
  end

  local line_count = #content

  if ifndef_index and define_index and endif_index and ifndef_match == define_match and define_index == ifndef_index + 1 then
    content[ifndef_index] = "#pragma once"
    content[define_index] = nil
    content[endif_index] = nil
  else
    print(filename .. " skipped: required pattern not found or in incorrect order")
    return
  end

  file = io.open(filename, "w")
  for i=1,line_count do
    local line = content[i]
    if line then
      file:write(line .. "\n")
    end
  end
  file:close()
  print(filename .. " updated")
end

for i = 1, #arg do
  replace_include_guard(arg[i])
end
