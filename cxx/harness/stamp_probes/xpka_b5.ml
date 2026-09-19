type z = int
let f () =
  let module S = Set.Make (String) in
  (module S : Set.S with type elt = string)
