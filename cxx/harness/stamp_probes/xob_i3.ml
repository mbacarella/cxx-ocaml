module type S = sig
  open Set.Make(Bool)
  val v : t
end
let x = 1
