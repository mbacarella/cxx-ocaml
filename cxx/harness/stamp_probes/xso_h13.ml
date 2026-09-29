module type S = sig
  open Set.Make(Bool)
  type u = private t
end
let x = 1
