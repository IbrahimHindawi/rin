if vim.b.rasm_lsp_started then
  return
end

if not (vim.lsp and vim.lsp.start) then
  return
end

-- Resolution order, most specific first:
--   1. vim.g.rasm_lsp_command, for anyone who wants to override it outright
--   2. $RASM_HOME/editors/lsp/rasm_lsp.py
--   3. the repo path recorded by editors/nvim/install.py at install time
-- Returns the command, or nil and the paths tried when no server script exists.
local function default_command()
  local tried = {}

  local rasm_home = vim.env.RASM_HOME
  if rasm_home and rasm_home ~= "" then
    local packaged = rasm_home .. "/editors/lsp/rasm_lsp.py"
    if vim.fn.filereadable(packaged) == 1 then
      return { "python", "-u", packaged }
    end
    table.insert(tried, packaged)
  end

  -- install.py rewrites this placeholder with the repo it was run from.
  local repo = "@RASM_REPO@"
  if not repo:find("^@") then
    local script = repo .. "/editors/lsp/rasm_lsp.py"
    if vim.fn.filereadable(script) == 1 then
      return { "python", "-u", script }
    end
    table.insert(tried, script)
  end

  -- Not installed: this file is on the runtimepath straight from the repo, at
  -- editors/nvim/after/ftplugin/rasm.lua.
  local here = debug.getinfo(1, "S").source:gsub("^@", "")
  local from_here = vim.fn.fnamemodify(here, ":p:h:h:h:h") .. "/lsp/rasm_lsp.py"
  from_here = from_here:gsub("\\", "/")
  if vim.fn.filereadable(from_here) == 1 then
    return { "python", "-u", from_here }
  end
  table.insert(tried, from_here)

  return nil, tried
end

local command, tried = vim.g.rasm_lsp_command, nil
if not command then
  command, tried = default_command()
end
if not command then
  -- The server is optional: highlighting works without it. Say so once.
  if not vim.g.rasm_lsp_missing_reported then
    vim.g.rasm_lsp_missing_reported = true
    vim.notify("rasm-lsp: no rasm_lsp.py found (tried " .. table.concat(tried, ", ")
      .. "); set vim.g.rasm_lsp_command or $RASM_HOME", vim.log.levels.INFO)
  end
  vim.b.rasm_lsp_started = false
  return
end
if type(command) == "string" then
  command = { command }
end

local root_markers = { ".git" }
local function find_root()
  if vim.fs and vim.fs.root then
    return vim.fs.root(0, root_markers)
  end

  local file = vim.api.nvim_buf_get_name(0)
  local dir = file ~= "" and vim.fn.fnamemodify(file, ":p:h") or vim.fn.getcwd()
  for _, marker in ipairs(root_markers) do
    local found = vim.fn.finddir(marker, dir .. ";")
    if found ~= "" then
      return vim.fn.fnamemodify(found, ":p:h:h")
    end
    found = vim.fn.findfile(marker, dir .. ";")
    if found ~= "" then
      return vim.fn.fnamemodify(found, ":p:h")
    end
  end

  return nil
end

local root = find_root()
if not root then
  local file = vim.api.nvim_buf_get_name(0)
  root = file ~= "" and vim.fn.fnamemodify(file, ":p:h") or vim.fn.getcwd()
end

if vim.diagnostic then
  pcall(vim.diagnostic.config, {
    underline = true,
    signs = true,
    virtual_text = false,
    update_in_insert = true,
    severity_sort = true,
  }, 0)
  if vim.diagnostic.enable then
    if not pcall(vim.diagnostic.enable, true, { bufnr = 0 }) then
      pcall(vim.diagnostic.enable, 0)
    end
  end
end

local get_clients = vim.lsp.get_clients or vim.lsp.get_active_clients
if get_clients then
  local ok, clients = pcall(get_clients, { name = "rasm-lsp" })
  if not ok then
    clients = get_clients()
  end

  for _, client in ipairs(clients or {}) do
    if client.name == "rasm-lsp" and client.config and client.config.root_dir == root then
      vim.lsp.buf_attach_client(0, client.id)
      vim.b.rasm_lsp_started = true
      return
    end
  end
end

local client_id = vim.lsp.start({
  name = "rasm-lsp",
  cmd = command,
  root_dir = root,
  cmd_cwd = root,
  flags = {
    debounce_text_changes = 75,
  },
})

if client_id then
  vim.b.rasm_lsp_started = true
else
  vim.b.rasm_lsp_started = false
  vim.notify("failed to start rasm-lsp", vim.log.levels.WARN)
end
