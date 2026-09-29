module type ORD = sig type t end
module type SET = sig type elt type t val iter : (elt -> unit) -> t -> unit
  end
module B (F : functor (X : ORD) -> SET) (Elt : ORD) = struct
  module M = struct module Z = F(Elt) end
end
