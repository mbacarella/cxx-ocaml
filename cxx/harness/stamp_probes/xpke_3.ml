module type E = sig exception Ex end let f (module M : E) = try raise
  Not_found with M.Ex -> 1 | _ -> 2 let g (module M : E) (M.Ex | _) = ()
