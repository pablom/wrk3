-- example HTTP POST script which demonstrates setting the
-- HTTP method, body, and adding a header

wrk.method = "POST"
wrk.path = "/notify"

wrk.body   = "amount=1.00&currency=USD"
wrk.headers["Content-Type"] = "application/x-www-form-urlencoded"
wrk.headers["Accept"] = "*/*"
wrk.headers["User-Agent"] = "wrk"


