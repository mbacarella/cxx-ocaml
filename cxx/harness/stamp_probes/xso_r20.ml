module type S = sig
  open Set.Make(Bool)
  type u = t
  open Set.Make(Bool)
  type v = t
end
let x = 1
