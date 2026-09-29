module X = struct let w = "s" end
let f () = let module X = struct let w = 1 end in X.w
let g () = X.w
