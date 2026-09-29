type z = int
let f (type s) set =
  let module Set = (val set : Set.S with type elt = s) in
  ()
