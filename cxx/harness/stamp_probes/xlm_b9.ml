let f () =
   let module N = Set.Make(String) in
   let s = N.empty in
   N.add "a" s
