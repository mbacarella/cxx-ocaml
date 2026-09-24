module type S = sig
  val x : int [@@ocaml.doc "d"] [@@inline never]
end
