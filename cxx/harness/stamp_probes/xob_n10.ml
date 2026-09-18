open Set.Make(Int)
let e = empty
let f = let module S = Set.Make(Int) in S.cardinal S.empty
