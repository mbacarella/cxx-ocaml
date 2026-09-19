type z = int
let sort (type s) set l =
  let module Set = (val set : Set.S with type elt = s) in
  Set.elements (List.fold_right Set.add l Set.empty)
let sort2 (type s) set l =
  let module Set = (val set : Set.S with type elt = s) in
  Set.elements (List.fold_right Set.add l Set.empty)
