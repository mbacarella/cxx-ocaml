module type S = sig
  open Set.Make(Bool)
  type u
  type v = u * t
end
let x = 1
