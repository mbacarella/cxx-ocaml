module type S = sig
  open Set.Make(Bool)
  type u = t
  type v = Set.Make(Bool).t
end
let x = 1
