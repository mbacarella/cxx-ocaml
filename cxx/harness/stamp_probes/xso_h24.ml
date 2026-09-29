module type S = sig
  open Set.Make(Bool)
  type u = t
  type v = t
end
let x = 1
