module type S = sig
  open Ephemeron.K2.Make(Bool)(Bool)
  type u = int t
end
let x = 1
