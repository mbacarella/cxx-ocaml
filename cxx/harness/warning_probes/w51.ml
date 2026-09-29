let rec f x = if x = 0 then 0 else 1 + (f[@tailcall]) (x - 1)
