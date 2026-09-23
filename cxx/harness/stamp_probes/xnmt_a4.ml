module type S = sig
  val f : int list -> int list
  val g : float -> float
end
module A : S = struct
  let f = List.map succ
  let g x = x
end
let u = A.f []
let v = A.g 0.0
