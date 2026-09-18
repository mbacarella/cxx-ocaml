let f () =
   let module N = Set.Make(String) in
   let s = N.empty in
   let g () = s in g
