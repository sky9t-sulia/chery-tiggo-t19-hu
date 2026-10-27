-- Terminal: the HU screen becomes a Linux console (root sh on the 2nd console), input - USB keyboard. exit - back to the shell.
-- Works only from NAND (/mnt/data1/hu-apps): sys.console() does not give root to programs from a USB stick.
local err = nil

function draw(w, h)
  ui.text("Terminal", w / 2, h / 2 - 130, { size = 38, weight = 700, align = "center" })
  ui.text("Подключи USB-клавиатуру. Выход из консоли - exit.", w / 2, h / 2 - 80,
          { size = 21, weight = 500, color = "#C7C7CC", align = "center" })
  ui.button("open", w / 2 - 160, h / 2 - 20, 320, 72, { label = "Открыть консоль", size = 26, weight = 700,
                                                        bg = "accent", fg = "on_accent", radius = 36 })
  if err then ui.text(err, w / 2, h / 2 + 100, { size = 20, weight = 600, color = "#FF453A", align = "center" }) end
end

function tap(id)
  if id == "open" then
    err = nil
    if not (sys and sys.console and sys.console()) then
      err = "Нельзя: программа должна лежать в NAND (/mnt/data1/hu-apps)"
    end
  end
end
