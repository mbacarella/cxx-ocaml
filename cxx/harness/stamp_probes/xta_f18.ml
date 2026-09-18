module IS = Set.Make(Int)
let f () = let module T = IS in T.mem
