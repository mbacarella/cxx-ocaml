module type S = sig
  open Set.Make(Bool)
  type u = elt
end
let x = 1
