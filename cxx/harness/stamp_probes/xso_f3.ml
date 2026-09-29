type u = Set.Make(Bool).t
module type S = sig
  val v : Set.Make(Bool).t
end
