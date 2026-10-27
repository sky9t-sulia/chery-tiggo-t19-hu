-- Calculator: Car Media v22 design (calcVals). Expression with parentheses, ⌫, history. Logic and look as in the design.

local calc = { ex = "", done = false, res = nil, hist = {} }

local function finite(n) return n == n and n ~= math.huge and n ~= -math.huge end

-- String(number) as in JS
local function jsstr(x)
  if x ~= x then return "NaN" end
  if x == math.huge then return "Infinity" end
  if x == -math.huge then return "-Infinity" end
  if x == 0 then return "0" end
  local s
  for p = 0, 16 do
    s = string.format("%." .. p .. "e", x)
    if tonumber(s) == x then break end
  end
  local sign, d1, ds, e = s:match("^(%-?)(%d)%.?(%d*)e([-+]%d+)$")
  local digits = (d1 .. ds):gsub("0+$", "")
  if digits == "" then digits = "0" end
  local k, n = #digits, tonumber(e) + 1
  local r
  if k <= n and n <= 21 then r = digits .. string.rep("0", n - k)
  elseif 0 < n and n <= 21 then r = digits:sub(1, n) .. "." .. digits:sub(n + 1)
  elseif -6 < n and n <= 0 then r = "0." .. string.rep("0", -n) .. digits
  else
    local ex = n - 1
    r = digits:sub(1, 1) .. (k > 1 and "." .. digits:sub(2) or "") .. "e" .. (ex < 0 and "-" or "+") .. math.abs(ex)
  end
  return sign .. r
end

local function prec12(n) return tonumber(string.format("%.11e", n)) end                                     -- toPrecision(12)
local function exp6(n) local s = string.format("%.6e", n); return (s:gsub("e([-+])0*(%d)", "e%1%2")) end   -- toExponential(6)

local function fmtN(n)
  if not finite(n) then return "Error" end
  local r = jsstr(prec12(n))
  if #(r:gsub("%-", "", 1)) > 12 then r = exp6(n) end
  return r
end

local function pretty(x) return (x:gsub("%*", "×"):gsub("/", "÷"):gsub("%-", "−")) end

-- expression parsing: + - * /, parentheses, unary minus, "2(3)" = 2*3; error - error()
local function evalEx(src)
  local i = 1
  local expr
  local function peek() return src:sub(i, i) end
  local function num()
    local m = src:match("^%d*%.?%d+", i) or src:match("^%d+%.?", i)
    if not m then error(0) end
    i = i + #m
    return tonumber(m) + 0.0
  end
  local function factor()
    if peek() == "-" then i = i + 1; return -factor() end
    if peek() == "(" then
      i = i + 1
      local v = expr()
      if peek() == ")" then i = i + 1 end
      return v
    end
    return num()
  end
  local function term()
    local v = factor()
    while true do
      local ch = peek()
      if ch == "*" then i = i + 1; v = v * factor()
      elseif ch == "/" then i = i + 1; v = v / factor()
      elseif ch == "(" then v = v * factor()
      else return v end
    end
  end
  expr = function()
    local v = term()
    while true do
      local ch = peek()
      if ch == "+" then i = i + 1; v = v + term()
      elseif ch == "-" then i = i + 1; v = v - term()
      else return v end
    end
  end
  local v = expr()
  if i <= #src then error(0) end
  return v
end

local function depth(x) local _, o = x:gsub("%(", ""); local _, c = x:gsub("%)", ""); return o - c end
local function close(x) return x .. string.rep(")", math.max(0, depth(x))) end
local function tryEval(x)
  if x == "" or x:find("[%+%-%*/%(]$") then return nil end
  local ok, v = pcall(evalEx, close(x))
  return ok and v or nil
end
local function set(p) for k, v in pairs(p) do calc[k] = v end end
local function isOp(ch) return ch ~= "" and ("+-*/"):find(ch, 1, true) ~= nil end
local function numTail(x) return x:match("[%d.]*$") end

local function digit(d)
  local q = calc
  local x = q.done and "" or q.ex
  if x:sub(-1) == ")" then x = x .. "*" end
  if d == "." then
    local seg = numTail(x)
    if seg:find(".", 1, true) then return end
    if seg == "" then x = x .. "0" end
  elseif numTail(x) == "0" then x = x:sub(1, -2) end
  if #(numTail(x):gsub("%.", "", 1)) >= 12 then return end
  set({ ex = x .. d, done = false })
end

local function op(o)
  local q = calc
  local x
  if q.done then x = (q.res ~= nil and finite(q.res)) and fmtN(q.res) or "" else x = q.ex end
  local last = x:sub(-1)
  if x == "" or last == "(" then
    if o == "-" then set({ ex = x .. "-", done = false }) end
    return
  end
  if isOp(last) then
    if x:sub(-2, -2) == "(" or #x == 1 then return end
    x = x:sub(1, -2)
  end
  set({ ex = x .. o, done = false })
end

local function open()
  local x = calc.done and "" or calc.ex
  if x:sub(-1):find("[%d.)]") then x = x .. "*" end
  set({ ex = x .. "(", done = false })
end

local function shut()
  if calc.done then return end
  local x = calc.ex
  if depth(x) <= 0 or not x:sub(-1):find("[%d.)]") then return end
  set({ ex = x .. ")" })
end

local function eq()
  local q = calc
  if q.done or q.ex == "" then return end
  local x = q.ex
  while x ~= "" and (isOp(x:sub(-1)) or x:sub(-1) == "(") do x = x:sub(1, -2) end
  if x == "" then return end
  x = close(x)
  local ok, r = pcall(evalEx, x)
  if not ok then r = 0 / 0 end
  local h = {}
  for _, e in ipairs(q.hist) do h[#h + 1] = e end
  h[#h + 1] = { ex = x, res = r }
  while #h > 20 do table.remove(h, 1) end
  set({ ex = x, done = true, res = r, hist = h })
end

local function clear() set({ ex = "", done = false, res = nil, hist = {} }) end
local function back() if calc.done then clear() else set({ ex = calc.ex:sub(1, -2) }) end end

-- history above the expression: when done the last entry is the current result, not shown
local function history()
  local h, out = calc.hist, {}
  local last = calc.done and #h - 1 or #h
  for j = math.max(1, last - 19), last do out[#out + 1] = h[j] end
  return out
end

local function useHist(e)
  if not finite(e.res) then return end
  local v = fmtN(e.res)
  local x = calc.done and "" or calc.ex
  if x:sub(-1):find("[%d.)]") then x = x .. "*" end
  set({ ex = x .. (e.res < 0 and "(" .. v .. ")" or v), done = false })
end

local DARK = { bg = "rgba(255,255,255,0.10)", fg = "#F5F5F7" }
local FN = { bg = "rgba(255,255,255,0.22)", fg = "#F5F5F7" }
local ACC = { bg = "#FF9F0A", fg = "#FFFFFF" }

local function K(id, t, fn, sty, fw) return { id = id, t = t, fn = fn, sty = sty, fw = fw or 600 } end
local function D(d) return K(d, d, function() digit(d) end, DARK) end
local KEYS = {
  K("clear", "AC", clear, FN), K("open", "(", open, FN), K("shut", ")", shut, FN), K("/", "÷", function() op("/") end, ACC, 500),
  D("7"), D("8"), D("9"), K("*", "×", function() op("*") end, ACC, 500),
  D("4"), D("5"), D("6"), K("-", "−", function() op("-") end, ACC, 500),
  D("1"), D("2"), D("3"), K("+", "+", function() op("+") end, ACC, 500),
  K("back", "⌫", back, FN), D("0"), D("."), K("=", "=", eq, ACC, 500),
}

local actions = {}

-- window: padding 18 22 22, gap 22; on the right a 520 grid (4 columns, gap 12, 5 rows);
-- left, bottom to top: number (line-height 1), expression (min-height 30), history (mask: transparent from the top down to 40%)
function draw(w, h)
  local gx, gy, gh = w - 22 - 520, 18, h - 18 - 22
  local cw, rh = (520 - 3 * 12) / 4, (gh - 4 * 12) / 5
  actions = {}
  for n, k in ipairs(KEYS) do
    local col, row = (n - 1) % 4, (n - 1) // 4
    ui.button(k.id, gx + col * (cw + 12), gy + row * (rh + 12), cw, rh,
              { label = k.t, size = 32, weight = k.fw, bg = k.sty.bg, fg = k.sty.fg, radius = 22 })
    actions[k.id] = k.fn
  end

  local live = (not calc.done) and tryEval(calc.ex) or nil
  local disp
  if calc.done then disp = finite(calc.res) and pretty(fmtN(calc.res)) or "Error"
  else disp = calc.ex ~= "" and pretty(calc.ex) or "0" end
  local top = ""
  if calc.done then top = pretty(calc.ex) .. " ="
  elseif live ~= nil and calc.ex:gsub("^%-", ""):find("[%+%-%*/%(]") then top = "= " .. pretty(fmtN(live)) end
  local n = utf8.len(disp)
  local sz = n > 14 and 44 or n > 9 and 64 or 96

  local rx, bottom, maxw = gx - 22 - 12, h - 22 - 10, gx - 22 - 22 - 24
  ui.text(disp, rx, bottom - sz / 2, { size = sz, weight = 300, color = "#F5F5F7", align = "right", maxw = maxw,
                                        spacing = -0.03 * sz, tnum = true })
  local lh = ui.line_height(24, 600)
  local etop = bottom - sz - 6 - math.max(30, lh)
  ui.text(top, rx, etop + lh / 2, { size = 24, weight = 600, color = "#8E8E93", align = "right", tnum = true })

  local htop, hbot = gy, etop - 6
  local hh, l1, l2 = hbot - htop, ui.line_height(18, 600), ui.line_height(28, 500)
  local y = hbot
  local items = history()
  for j = #items, 1, -1 do
    local e = items[j]
    local ib = y - 4 - l2 - l1 - 4
    if y <= htop then break end
    local ex, res = pretty(e.ex) .. " =", finite(e.res) and pretty(fmtN(e.res)) or "Error"
    local function fade(cy) return math.max(0, math.min(1, (cy - htop) / (0.4 * hh))) end
    local c1, c2 = ib + 4 + l1 / 2, ib + 4 + l1 + l2 / 2
    if c1 > htop then ui.text(ex, rx, c1, { size = 18, weight = 600, color = ("rgba(110,110,115,%.3f)"):format(fade(c1)), align = "right", maxw = maxw, tnum = true }) end
    ui.text(res, rx, c2, { size = 28, weight = 500, color = ("rgba(174,174,178,%.3f)"):format(fade(c2)), align = "right", maxw = maxw, tnum = true })
    local bw = math.min(maxw, math.max(ui.text_width(ex, 18, 600, true), ui.text_width(res, 28, 500, true))) + 20
    local by = math.max(ib, htop)
    ui.button("h" .. j, rx + 10 - bw, by, bw, y - by, { bg = "rgba(0,0,0,0)", radius = 12 })
    actions["h" .. j] = function() useHist(e) end
    y = ib - 4
  end
end

function tap(id)
  local f = actions[id]
  if f then f() end
end
