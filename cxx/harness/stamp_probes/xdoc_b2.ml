module type S = sig
  val x : int [@@doc "d"] [@@text "e"] [@@inline never]
end
