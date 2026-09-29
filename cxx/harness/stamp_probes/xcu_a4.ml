module M : Set.S with type elt = int = Set.Make (Int)
let e = M.empty
