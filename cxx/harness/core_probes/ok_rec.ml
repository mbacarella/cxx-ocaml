let rec fact n = if n = 0 then 1 else n * fact (n - 1)
let rec even n = n = 0 || odd (n - 1) and odd n = n <> 0 && even (n - 1)
