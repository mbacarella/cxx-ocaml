module type S = sig
  open Set.Make(Bool)
  type nonrec t = t
  val v : t
end
let x = 1
