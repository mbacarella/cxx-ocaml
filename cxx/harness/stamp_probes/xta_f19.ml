module TT = struct module IS = Set.Make(Int) end
let f () = let module T = TT in T.IS.empty
