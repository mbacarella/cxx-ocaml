let f () =
   let module N = Map.Make(String) in
   let _ = N.cardinal in ()
