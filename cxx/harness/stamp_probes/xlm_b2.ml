let f () =
   let module N = Set.Make(String) in
   N.add "a" N.empty
