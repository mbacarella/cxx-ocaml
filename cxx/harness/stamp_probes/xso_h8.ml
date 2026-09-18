module type S = sig
  open Set.Make(Bool)
  type u = int -> t
end
let x = 1
