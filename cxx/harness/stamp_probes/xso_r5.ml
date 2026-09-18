module type S = sig
  open Set.Make(Stdlib__Bool)
  type u = t
end
let x = 1
