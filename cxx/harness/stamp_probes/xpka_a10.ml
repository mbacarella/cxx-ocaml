type z = int
let f (type s) cmp =
  let module S = Set.Make (struct type t = s let compare = cmp end) in
  let m = (module S : Set.S with type elt = s) in m
