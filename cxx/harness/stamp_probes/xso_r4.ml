module type S = sig
  open Weak.Make(Bool)
  type u = t
end
let x = 1
