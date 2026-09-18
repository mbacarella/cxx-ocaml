let f () =
   let module N = Map.Make(String) in
   let s = N.empty in
   let g () = s in g
