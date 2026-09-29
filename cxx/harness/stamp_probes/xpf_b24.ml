module type ORD = sig type t end
module type SET = sig type elt type t val iter : (elt -> unit) -> t -> unit
  end
module type FT = functor (X : ORD) -> SET
module B (F : FT) (Elt : ORD) = struct
  module Z = F(Elt)
end
