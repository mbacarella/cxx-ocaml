module type S = sig
  open Set.Make(Bool)
  val v : Set.Make(Bool).t
end
let x = 1
