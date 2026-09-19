type z = int
let f () =
  let module S = Set.Make (Int) in
  (module S : Set.S with type elt = int), S.empty
