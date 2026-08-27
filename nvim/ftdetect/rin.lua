-- Filetype detection for the rin language.
--
-- This used to need a workaround: Neovim's built-in filetype table claims *.i
-- for Progress, so a plain autocmd raced against it. The .rin extension is not
-- claimed by anything built in, so the mapping below is enough on its own.
-- vim.filetype.add() is still used in preference to an autocmd because it is
-- consulted before the built-in table, which keeps the result deterministic if
-- that ever changes.

vim.filetype.add({
  extension = {
    rin = "rin",
  },
})

-- Fallback for the case where this file is sourced after a buffer already exists
-- (for example when installing while Neovim is running).
vim.api.nvim_create_autocmd({ "BufRead", "BufNewFile" }, {
  group = vim.api.nvim_create_augroup("rin_filetype", { clear = true }),
  pattern = "*.rin",
  callback = function(args)
    if vim.bo[args.buf].filetype ~= "rin" then
      vim.bo[args.buf].filetype = "rin"
    end
  end,
})
