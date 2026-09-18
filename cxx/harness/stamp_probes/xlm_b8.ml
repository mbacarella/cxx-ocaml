let f () =
   let module N = Set.Make(String) in
   let s = N.empty in
   ignore (N.add "a" s)
