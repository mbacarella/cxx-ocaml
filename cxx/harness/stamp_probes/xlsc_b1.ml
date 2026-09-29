module X = struct let w = "s" end
let f () = let module Y = struct let w = 1 end in Y.w
let g () = X.w
