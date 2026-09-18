open Set.Make(Int)
let x = 1
module type S = sig
  val v : Set.Make(Bool).t
end
