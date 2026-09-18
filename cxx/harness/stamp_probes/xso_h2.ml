module type S = sig
  open Set.Make(Bool)
  type u = t list
end
let x = 1
