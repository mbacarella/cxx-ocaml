module S = String
let f () =
   let module N = Map.Make(S) in
   N.add "sum" 41 N.empty
