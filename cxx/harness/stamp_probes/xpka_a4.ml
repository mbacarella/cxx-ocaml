type z = int
let f cmp =
  let module S = Set.Make (struct type t = int let compare = cmp end) in
  (module S : Set.S with type elt = int)
