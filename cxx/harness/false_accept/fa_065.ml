module M = struct let f x = x + 1 end let y = let open M in f "a"
