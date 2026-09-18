let f () =
   let module N = Set.Make(String) in
   N.mem "a"
