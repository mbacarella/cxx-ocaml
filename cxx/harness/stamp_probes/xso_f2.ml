module M = Set.Make(Bool)
module type S = sig
  val v : Set.Make(Bool).t
end
