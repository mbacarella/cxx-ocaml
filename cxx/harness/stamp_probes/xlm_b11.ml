let f () =
   let module N = Map.Make(String) in
   let g (x : int N.t) = x in g
