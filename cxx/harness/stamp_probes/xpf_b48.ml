module type ORD = sig type t end
module type SET = sig type elt type t val iter : (elt -> unit) -> t -> unit
  end
module B (F : functor (X : ORD) -> SET with type t := X.t) (Elt : ORD) =
  struct
  module Z = F(Elt)
end
