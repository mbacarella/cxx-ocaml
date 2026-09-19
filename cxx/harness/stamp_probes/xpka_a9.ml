type z = int
let f (type s) (cmp : s -> s -> int) =
  let module S = Set.Make (struct type t = s let compare = cmp end) in
  (module S : Set.S with type elt = s)
