
require("bit")

function htons(x)
    return bit.bor(
        bit.rshift( x, 8 ),
        bit.band( bit.lshift( x, 8 ), 0xFF00 )
    )

end

request_body  = "Hello world"

request = function()
	local len = htons(string.len(request_body))
   	return request_body
end

response = function(body)
   if request_body == body then
        print('success')
        return 200
   end
   print('error')
   return 300
end
