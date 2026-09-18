let f () =
   let module S = String in
   let module N = Set.Make(S) in
   N.add "sum" N.empty
