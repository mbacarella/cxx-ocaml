module type S = sig
  val v : Weak.Make(Bool).t
end
let x = 1
