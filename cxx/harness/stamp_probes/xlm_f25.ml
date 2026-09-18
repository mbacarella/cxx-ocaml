let f () =
   let module N = Set.Make(String) in
   let module M = Map.Make(String) in
   M.add "a" N.empty M.empty
