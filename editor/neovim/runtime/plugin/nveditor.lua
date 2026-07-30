if vim.g.loaded_kalam_nveditor == 1 then
  return
end
vim.g.loaded_kalam_nveditor = 1

require("nveditor").setup()
