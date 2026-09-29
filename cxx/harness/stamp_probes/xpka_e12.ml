type z = int
let f () =
  let module S = Set.Make (Int) in
  let module T = struct include S end in
  (module T : Set.S with type elt = int)
