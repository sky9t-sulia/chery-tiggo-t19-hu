-- Hello World: example Lua program (text, system button, touches, accent color).
local taps = 0
local finger = nil      -- where the finger is (touch)

function draw(w, h)
  ui.text("Hello, World!", w / 2, h / 2 - 90, { size = 56, weight = 800, align = "center" })
  ui.text("Нажато: " .. taps, w / 2, h / 2 - 30, { size = 24, weight = 600, color = "#C7C7CC", align = "center", tnum = true })
  ui.button("tap", w / 2 - 130, h / 2 + 20, 260, 72, { label = "Нажми меня", size = 28, weight = 800,
                                                       bg = "accent", fg = "on_accent", radius = 36 })
  if finger then ui.rect(finger.x - 20, finger.y - 20, 40, 40, "rgba(255,255,255,0.25)", 20) end
end

function tap(id)
  if id == "tap" then taps = taps + 1 end
end

function touch(phase, x, y)
  finger = phase ~= "up" and { x = x, y = y } or nil
end
