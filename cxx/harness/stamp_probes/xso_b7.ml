module type S = sig
  open Weak.Make(Bool)
end
let x = 1
