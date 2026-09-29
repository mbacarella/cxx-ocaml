type z = int
let make_set (type s) cmp =
  let module S = Set.Make(struct type t = s let compare = cmp end) in
  (module S : Set.S with type elt = s)
let make_set2 (type s) cmp =
  let module S = Set.Make(struct type t = s let compare = cmp end) in
  (module S : Set.S with type elt = s)
