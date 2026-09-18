let f () =
   let module N = Set.Make(String) in
   fun s -> N.cardinal s
