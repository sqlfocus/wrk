--[[
1. wrk.request()句柄通过闭包的概念，存储本线程的请求报文
]]

--[[此模块儿的局部表，最后将被暴露出去]]
local wrk = {
   scheme  = "http",              --[[默认值将在script_create()被赋值]]
   host    = "localhost",
   port    = nil,
   method  = "GET",
   path    = "/",
   headers = {},
   body    = nil,
   thread  = nil,
}

--[[
     DNS解析入口，调用的wrk.lookup为C函数script_wrk_lookup()
  ]]
function wrk.resolve(host, service)
   local addrs = wrk.lookup(host, service)
   for i = #addrs, 1, -1 do
      if not wrk.connect(addrs[i]) then
         table.remove(addrs, i)
      end
   end
   wrk.addrs = addrs
end

--[[
     选取第一个可用IP地址，做为线程的地址；
     并调用全局函数setup()
  ]]
function wrk.setup(thread)
   thread.addr = wrk.addrs[1]
   if type(setup) == "function" then
      setup(thread)
   end
end

--[[
     利用配置参数wrk.host+wrk.port初始化wrk.headers["Host"]，
     并调用全局函数init()，后续利用wrk.format()生成请求报文
     最后注册wrk.request()函数，返回请求报文
  ]]
function wrk.init(args)
   if not wrk.headers["Host"] then
      local host = wrk.host
      local port = wrk.port

      host = host:find(":") and ("[" .. host .. "]")  or host
      host = port           and (host .. ":" .. port) or host

      wrk.headers["Host"] = host
   end

   if type(init) == "function" then
      init(args)
   end

   local req = wrk.format()
   wrk.request = function()
      return req
   end
end

--[[
     生成请求报文；存储在s中，以行为单位
     最后通过连接表元素的方式，形成最终的请求报文字符串
     @param: method, string, GET
     @param: path, string, HTTP请求的地址
     @param: headers, table, 定制化的HTTP头
     @param: body, string, 报文内容
     @ret: 请求报文字符串
  ]]
function wrk.format(method, path, headers, body)
   local method  = method  or wrk.method
   local path    = path    or wrk.path
   local headers = headers or wrk.headers
   local body    = body    or wrk.body
   local s       = {}

   if not headers["Host"] then
      headers["Host"] = wrk.headers["Host"]
   end

   headers["Content-Length"] = body and string.len(body)

   s[1] = string.format("%s %s HTTP/1.1", method, path)
   for name, value in pairs(headers) do
      s[#s+1] = string.format("%s: %s", name, value)
   end

   s[#s+1] = ""
   s[#s+1] = body or ""

   return table.concat(s, "\r\n")
end

return wrk
