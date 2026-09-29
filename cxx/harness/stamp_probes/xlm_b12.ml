let f () =
   let module N = Map.Make(String) in
   let g (x : int N.t) = N.cardinal x in g
