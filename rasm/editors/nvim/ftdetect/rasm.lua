-- Filetype detection for rasm, the x64 assembler with rin's declarations.
--
-- Nothing built in claims *.rasm, but vim.filetype.add() is used rather than an
-- autocmd because it is consulted before the built-in table, which keeps the
-- result deterministic if that ever changes (rin's .rin needed this).

vim.filetype.add({
  extension = {
    rasm = "rasm",
  },
})

-- Fallback for the case where this file is sourced after a buffer already exists
-- (for example when installing while Neovim is running).
vim.api.nvim_create_autocmd({ "BufRead", "BufNewFile" }, {
  group = vim.api.nvim_create_augroup("rasm_filetype", { clear = true }),
  pattern = "*.rasm",
  callback = function(args)
    if vim.bo[args.buf].filetype ~= "rasm" then
      vim.bo[args.buf].filetype = "rasm"
    end
  end,
})
