module S = String
module N = Map.Make(S)
let f () = N.add "sum" 41 N.empty
