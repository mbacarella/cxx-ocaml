module type S = sig
  val x : int
  (** doc *)
  [@@alert foo "x"]
end
