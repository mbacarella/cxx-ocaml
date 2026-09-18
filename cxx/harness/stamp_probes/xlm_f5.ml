let f () =
   let module N = Map.Make(String) in
   fun x -> N.cardinal x
