module type ORD = sig type t end
module type SET = sig type elt type t val iter : (elt -> unit) -> t -> unit
  end
module B (F : functor (X : ORD) -> SET) = struct
  module E = struct type t = int end
  module Z = F(E)
end
