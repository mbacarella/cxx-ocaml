module type S = sig
  open Set.Make(Bool)
  type nonrec elt = elt
end
let x = 1
