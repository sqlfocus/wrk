local wrk = require("wrk")

--[[发送请求前等待时间，ms]]
function delay()
    return math.random(1000, 2000)
end


--[[
     线程启动前被调用，用于定制线程初始化信息
     1) 定制此线程绑定的IP地址
  ]]
local thread_counter = 1             --[[用于线程计数，仿真线程ID]]
local interface_name = "lo"
local ip_addr_list = {eth2 = {}, eth3 = {}, lo = {}}
function setup(thread)
   --[[获取客户端应该绑定的网卡名:
           如果目的地址为172.168.142.100, 则网卡应该为eth0
	   如果目的地址为172.168.242.100, 则网卡应该为eth2
	   本机测试，127.0.0.1, 则网卡应该为lo]]
   if thread_counter == 1 then
       local sub_3rd_ip = wrk.host:sub(9, 9)
       if sub_3rd_ip == "1" then
           interface_name = "eth2"
       elseif sub_3rd_ip == "2" then
           interface_name = "eth3"
       else
           interface_name = "lo"
       end
       
       print("interface: " .. interface_name)
   end

   --[[获取网卡的地址列表]]
   if #ip_addr_list[interface_name] == 0 then
       local fp = io.popen("ip addr show " .. interface_name)
       for line in fp:lines() do
           line = line:match("^%s*(.-)%s*$")
           if line:sub(1,5) == "inet " then
	       local ip4_addr = line:match("%s(%d+%.%d+%.%d+%.%d+)/%d+%s")
               table.insert(ip_addr_list[interface_name], ip4_addr)
           end
       end
       for k,tb in pairs(ip_addr_list) do
           print("接口: " .. k .. ", count: " .. #ip_addr_list[k])
           for _,ip in pairs(ip_addr_list[k]) do
	       print("    " .. ip)
	   end
       end
   end
   
   --[[设置线程对应的IP地址]]
   if thread_counter > #ip_addr_list[interface_name] then
       print("too many thread, not enough IP, " .. thread_counter)
       return
   end
   thread:set("caddr", ip_addr_list[interface_name][thread_counter])

   --[[增加线程计数]]
   thread_counter = thread_counter + 1
end