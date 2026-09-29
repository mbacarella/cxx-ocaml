open Set.Make(Bool)
let e = empty
module type S = sig
  val v : Set.Make(Bool).t
end
