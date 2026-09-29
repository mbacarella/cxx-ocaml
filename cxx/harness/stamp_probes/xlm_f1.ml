let f () =
   let module N = Map.Make(String) in
   let open N in
   add "a" 1 empty
