module type S = sig
  open Set.Make(Bool)
  val v : t
  type u = t
end
let x = 1
