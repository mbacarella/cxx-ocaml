module type E = sig exception Ex of int end let f ((module M) : (module E))
  = function M.Ex n -> n | _ -> 0
