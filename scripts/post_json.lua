-- example HTTP POST script which demonstrates setting the
-- HTTP method, body, and adding a header

wrk.method = "POST"
wrk.path = "/v1"
wrk.body   = '{"id":1,"jsonrpc":"2.0","method":"echo","params":["Hello world"]}'
wrk.headers["Content-Type"] = "application/json"
wrk.headers["Accept"] = "*/*"
wrk.headers["User-Agent"] = "wrk"


