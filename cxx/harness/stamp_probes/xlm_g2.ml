let f () =
   let module N = Map.Make(Stdlib__String) in
   N.add "a" 41 N.empty
