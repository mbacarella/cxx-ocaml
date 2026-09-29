module type S = sig
  open Set.Make(Bool)
  val v : t -> t
end
let x = 1
