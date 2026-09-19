type z = int
let make_set (type s) cmp =
  let module S = Set.Make(struct type t = s let compare = cmp end) in
  (module S : Set.S with type elt = s)
let x : (module Set.S with type elt = int) = make_set compare
