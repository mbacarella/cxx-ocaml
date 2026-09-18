module type S = sig
  open Set.Make(Bool)
  type u = t and v = t
end
let x = 1
