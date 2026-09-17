module type ORD = sig type t end
module type SET = sig type elt type t val iter : (elt -> unit) -> t -> unit
  end
module B (F : functor (X : ORD) -> SET) (Elt : ORD) = struct
  module Z : SET = F(Elt)
end
