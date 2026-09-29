let f () =
   let module N = Set.Make(String) in
   let g (x : N.t) = x in g
