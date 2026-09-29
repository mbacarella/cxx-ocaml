module F (X : Set.OrderedType) = Set.Make (X)
let () = let module N = F (Int) in ()
