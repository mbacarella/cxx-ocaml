module type S = sig
  open Set.Make(Bool)
  exception E of t
  type u = t
end
let x = 1
