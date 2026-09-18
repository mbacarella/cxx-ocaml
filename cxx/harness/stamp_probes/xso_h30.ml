module type S = sig
  open Set.Make(Bool)
  type nonrec elt = elt
  type nonrec t = t
end
let x = 1
