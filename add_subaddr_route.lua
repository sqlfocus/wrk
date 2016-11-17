local op, device, ip_net = ...

if not op or not device then
   print("usage help: ")
   print("    lua change_addrs.lua <operation> <device-name> <net_part>")
   print("    ")
   print("    operation: add | del")
   print("    device-name: eth2 | eth3")
   print("    net-part: 172.168.10.")
   os.exit()
end

if device == "eth2" then
   router_dst = "172.168.142.0"
elseif device == "eth3" then
   router_dst = "172.168.242.0"
else
   print("wrong device name, " .. device)
   os.exit()
end

if op ~= "add" and op ~="del" then
    print("wrong operation, " .. op)
    os.exit()
end

--[[add ip addrs]]
for i=2,254 do
    os.execute("ip addr " .. op .. " " .. ip_net .. i .. "/24 dev " .. device)
end


--[[add router]]
if op == "add" then
    os.execute("ifconfig " .. device .. " up")
    os.execute("route add -net " .. router_dst .. "/24 gw " .. ip_net .. "1")
else
    os.execute("ifconfig " .. device .. " down")
    os.execute("route del -net " .. router_dst .. "/24 gw " .. ip_net .. "1")
end
		