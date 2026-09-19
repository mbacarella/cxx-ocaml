type z = int
let make_set (type s) cmp =
  let module S = Set.Make(struct type t = s let compare = cmp end) in
  (module S : Set.S with type elt = s)
let sort (type s) set l =
  let module Set = (val set : Set.S with type elt = s) in
  Set.elements (List.fold_right Set.add l Set.empty)
