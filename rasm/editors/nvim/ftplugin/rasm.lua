if vim.b.did_ftplugin then
  return
end
vim.b.did_ftplugin = true

-- `;` comments to the end of the line, as in every assembler.
vim.bo.commentstring = "; %s"
vim.bo.comments = ":;"
vim.bo.formatoptions = vim.bo.formatoptions:gsub("t", "") .. "croql"

-- The examples indent with four spaces: one level per `{`, labels at the level
-- of the code around them. smartindent follows `{` and `}` and puts a `#define`
-- in column 0; cinwords is emptied so no mnemonic triggers an extra indent.
vim.bo.expandtab = true
vim.bo.shiftwidth = 4
vim.bo.tabstop = 4
vim.bo.softtabstop = 4
vim.bo.autoindent = true
vim.bo.smartindent = true
vim.bo.cindent = false
vim.bo.indentexpr = ""
vim.bo.cinwords = ""

vim.b.undo_ftplugin = "setlocal commentstring< comments< formatoptions< expandtab< shiftwidth<"
  .. " tabstop< softtabstop< autoindent< smartindent< cindent< indentexpr< cinwords<"
  .. " | unlet! b:did_ftplugin b:rasm_lsp_started"

-- Start the language server even when only this directory, not its after/, is
-- on the runtimepath. after/ftplugin/rasm.lua guards against a second start.
vim.cmd("runtime after/ftplugin/rasm.lua")
