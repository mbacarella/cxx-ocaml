module type S = sig
  open Set.Make(Bool)
  val v : int
end
let x = 1
